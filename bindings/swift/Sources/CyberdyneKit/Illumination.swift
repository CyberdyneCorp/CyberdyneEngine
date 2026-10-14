// SPDX-License-Identifier: MIT
// Illumination.swift — ABI 1.9's global illumination, as a game writes it. Issue #35.
//
//     var settings = try Illumination.settings                     // a graphics options menu
//     settings.quality = .high
//     settings.hardwareRayTracing = false
//     try Illumination.apply(settings)
//
//     let crypt = try GIVolume.place(.init(                        // a level's important interior
//         bounds: (min: Vec3(x: -8, y: 0, z: -8), max: Vec3(x: 8, y: 4, z: 8)),
//         overrides: [.quality(.epic), .updateRate(2)]))
//     try Illumination.setClass(.fastDynamic, of: cart)            // it moves every frame
//     try Illumination.setEmission(.off, of: torch)                // the guard put the torch out
//
//     let light = try Illumination.irradiance(at: thief.position)  // onUpdate: how lit is the thief?
//     if light.luminance < 0.05 { hidden = true }
//
// The GI is the engine's `cy::rendering::gi::IlluminationSystem`, the one the host renders with.
//
// PHASES. Illumination is presentation: every call is refused in `onFixedUpdate` with
// `CyberdyneError.status(.permissionDenied, …)`, so the simulation never depends on the light and a
// resimulated tick never changes it twice. An AI that reacts to the light samples it in `onUpdate`
// and acts on what it read in the next fixed step, as it acts on a click.
//
// WHAT A SAMPLE IS. `irradiance(at:normal:)` is the engine's `indirect_diffuse` at that point: the
// mean cosine-weighted INDIRECT radiance arriving there, the light every surface's albedo
// multiplies. With no normal it is the mean over the six axis directions — what a body standing
// there receives on average. Direct light from the frame's lights is not in it.

import CyberdyneABI
import CyberdyneCore

/// The engine's global illumination, through ABI 1.9's `gi_*`.
public enum Illumination {
    /// The GI settings of the running world.
    public struct Settings: Equatable, Sendable {
        /// Which sources light the world.
        public var mode: GIMode
        /// The global probe quality; a GI volume's where one reaches.
        public var quality: GIQuality
        /// The global tier preference; a GI volume's where one reaches.
        public var tier: GITier
        /// Use hardware ray tracing where the device has it. Off is exactly a device without it.
        public var hardwareRayTracing: Bool
        /// The GI's allocation, in milliseconds.
        public var budgetMilliseconds: Float

        /// Settings; the engine's defaults unless given.
        public init(
            mode: GIMode = .dynamic, quality: GIQuality = .medium, tier: GITier = .automatic,
            hardwareRayTracing: Bool = true, budgetMilliseconds: Float = 3
        ) {
            self.mode = mode
            self.quality = quality
            self.tier = tier
            self.hardwareRayTracing = hardwareRayTracing
            self.budgetMilliseconds = budgetMilliseconds
        }

        /// From the ABI's spelling. An enumerator this module does not know reads as the default.
        public init(_ raw: CyGiSettings) {
            mode = GIMode(rawValue: raw.mode) ?? .dynamic
            quality = GIQuality(rawValue: raw.quality) ?? .medium
            tier = GITier(rawValue: raw.tier) ?? .automatic
            hardwareRayTracing = raw.flags & CY_GI_HARDWARE_RAY_TRACING != 0
            budgetMilliseconds = raw.budget_ms
        }

        /// In the ABI's spelling.
        public var raw: CyGiSettings {
            var out = CyGiSettings()
            out.struct_size = UInt32(MemoryLayout<CyGiSettings>.size)
            out.mode = mode.rawValue
            out.quality = quality.rawValue
            out.tier = tier.rawValue
            out.flags = hardwareRayTracing ? CY_GI_HARDWARE_RAY_TRACING : 0
            out.budget_ms = budgetMilliseconds
            return out
        }
    }

    /// The indirect light arriving at a point.
    public struct Irradiance: Equatable, Sendable {
        /// Mean cosine-weighted incoming indirect radiance, linear.
        public var radiance: Vec3
        /// Its Rec. 709 luminance: one number for "how lit".
        public var luminance: Float
        /// How much of the answer the caches stood behind, 0 to 1. Low right after a change.
        public var confidence: Float
        /// The source that carried most of it.
        public var source: GISource
        /// It was asked with no normal: the mean over six axis directions.
        public var omnidirectional: Bool

        /// From the ABI's spelling.
        public init(_ raw: CyGiIrradiance) {
            radiance = Vec3(raw.radiance)
            luminance = raw.luminance
            confidence = raw.confidence
            source = GISource(rawValue: raw.source) ?? .none
            omnidirectional = raw.flags & CY_GI_SAMPLE_OMNIDIRECTIONAL != 0
        }
    }

    /// What the GI did on the last frame.
    public struct Status: Equatable, Sendable {
        /// The tier that answered world rays: software or hardware tracing, or none.
        public var worldTier: GISource
        /// The least converged region, 0 to 1.
        public var convergence: Float
        /// The GI's own cost on the last frame, and what it was allowed, in milliseconds.
        public var measuredMilliseconds: Float
        public var allocationMilliseconds: Float
        /// GI volumes placed, objects registered, emitter cards lighting as lights.
        public var volumes: UInt32
        public var objects: UInt32
        public var promotedLights: UInt32
        /// Emitters classified, and how many of them are switched off.
        public var emitters: UInt32
        public var disabledEmitters: UInt32
        /// The frame the GI last ran.
        public var frame: UInt64

        /// From the ABI's spelling.
        public init(_ raw: CyGiStatus) {
            worldTier = GISource(rawValue: raw.world_tier) ?? .none
            convergence = raw.convergence
            measuredMilliseconds = raw.measured_ms
            allocationMilliseconds = raw.allocation_ms
            volumes = raw.volumes
            objects = raw.objects
            promotedLights = raw.promoted_lights
            emitters = raw.emitters
            disabledEmitters = raw.disabled_emitters
            frame = raw.frame
        }
    }

    /// The running world's GI settings.
    public static var settings: Settings {
        get throws {
            var raw = CyGiSettings()
            raw.struct_size = UInt32(MemoryLayout<CyGiSettings>.size)
            try GameServices.engine().giGetSettings(into: &raw)
            return Settings(raw)
        }
    }

    /// Apply `settings` from the next frame on. A change of quality or tier places every probe
    /// again, so the light converges again over a few frames; hardware ray tracing switches
    /// without a rebuild.
    public static func apply(_ settings: Settings) throws {
        var raw = settings.raw
        try GameServices.engine().giSetSettings(settings: &raw)
    }

    /// How `entity` takes part in the GI. `.automatic` returns it to the class its mobility and
    /// size give it. Throws `.notFound` for an entity the host did not register with the GI.
    public static func setClass(_ klass: GIClass, of entity: Entity) throws {
        try GameServices.engine().giObjectSetClass(entity: entity.bits, klass: klass.rawValue)
    }

    /// The class `entity` has now, after any override.
    public static func illuminationClass(of entity: Entity) throws -> GIClass {
        var raw: UInt32 = 0
        try GameServices.engine().giObjectClass(entity: entity.bits, into: &raw)
        return GIClass(rawValue: raw) ?? .automatic
    }

    /// What the GI makes of `entity`'s emission: `.off` puts a lamp out, `.automatic` gives the
    /// decision back to its power and size. The rule for an entity beats a material's.
    public static func setEmission(_ mode: GIEmission, of entity: Entity) throws {
        try GameServices.engine().giEmissionSet(entity: entity.bits, material: 0, mode: mode.rawValue)
    }

    /// What the GI makes of every emitter drawn with the material called `material`, or — with
    /// `entity` — of that entity's in that material alone.
    public static func setEmission(
        _ mode: GIEmission, ofMaterial material: String, on entity: Entity = .null
    ) throws {
        try GameServices.engine().giEmissionSet(
            entity: entity.bits, material: AnimationName.hash(material), mode: mode.rawValue)
    }

    /// The indirect light arriving at `position`: about `normal` for a surface, or with no normal
    /// the six-direction mean a body standing there receives. Throws for a non-finite point.
    public static func irradiance(at position: Vec3, normal: Vec3? = nil) throws -> Irradiance {
        let engine = try GameServices.engine()
        var raw = CyGiIrradiance()
        raw.struct_size = UInt32(MemoryLayout<CyGiIrradiance>.size)
        let point = [position.x, position.y, position.z]
        try point.withUnsafeBufferPointer { point in
            try withOptionalBuffer(normal.map { [$0.x, $0.y, $0.z] }) { normal in
                try engine.giSampleIrradiance(position: point.baseAddress, normal: normal, into: &raw)
            }
        }
        return Irradiance(raw)
    }

    /// What the GI did on the last frame.
    public static var status: Status {
        get throws {
            var raw = CyGiStatus()
            raw.struct_size = UInt32(MemoryLayout<CyGiStatus>.size)
            try GameServices.engine().giStatus(into: &raw)
            return Status(raw)
        }
    }
}

/// A placed GI volume: a box that overrides the GI's settings inside it, fading in over
/// `blendDistance` outside it. Overlapping volumes resolve by priority.
public struct GIVolume: Hashable, Sendable {
    /// The engine's handle. Never reused, so a destroyed volume's is refused with `.notFound`.
    public let handle: CyGiVolume

    /// One setting a volume overrides.
    public enum Override: Equatable, Sendable {
        /// The probe ray multiplier.
        case quality(GIQuality)
        /// Above 1 full probe density, below 1 the sparse lattice only.
        case probeDensity(Float)
        /// How fast its probes age in the scheduler: 2 is about twice the updates.
        case updateRate(Float)
        /// Scales the indirect diffuse light.
        case diffuseIntensity(Float)
        /// Scales the indirect specular light.
        case specularIntensity(Float)
        /// Which world tier its rays prefer.
        case tier(GITier)
        /// Metres from the camera at which the far field begins.
        case farField(Float)
    }

    /// What a volume is.
    public struct Description: Equatable, Sendable {
        /// Its box, in world space.
        public var bounds: (min: Vec3, max: Vec3)
        /// What it overrides; nothing else is touched.
        public var overrides: [Override]
        /// Higher wins where volumes overlap.
        public var priority: Int32
        /// Metres outside the box it fades over; zero switches at the boundary.
        public var blendDistance: Float
        /// 0 to 1: dials the whole volume down without moving it.
        public var weight: Float

        /// A volume over `bounds`.
        public init(
            bounds: (min: Vec3, max: Vec3), overrides: [Override], priority: Int32 = 0,
            blendDistance: Float = 1, weight: Float = 1
        ) {
            self.bounds = bounds
            self.overrides = overrides
            self.priority = priority
            self.blendDistance = blendDistance
            self.weight = weight
        }

        public static func == (lhs: Description, rhs: Description) -> Bool {
            lhs.bounds.min == rhs.bounds.min && lhs.bounds.max == rhs.bounds.max
                && lhs.overrides == rhs.overrides && lhs.priority == rhs.priority
                && lhs.blendDistance == rhs.blendDistance && lhs.weight == rhs.weight
        }

        /// In the ABI's spelling: each override's field and its bit in `sets`.
        public var raw: CyGiVolumeDesc {
            var out = CyGiVolumeDesc()
            out.struct_size = UInt32(MemoryLayout<CyGiVolumeDesc>.size)
            out.bounds_min = bounds.min.tuple
            out.bounds_max = bounds.max.tuple
            out.priority = priority
            out.blend_distance = blendDistance
            out.weight = weight
            for override in overrides {
                switch override {
                case .quality(let quality):
                    out.sets |= CY_GI_VOLUME_SETS_QUALITY
                    out.quality = quality.rawValue
                case .probeDensity(let density):
                    out.sets |= CY_GI_VOLUME_SETS_PROBE_DENSITY
                    out.probe_density = density
                case .updateRate(let rate):
                    out.sets |= CY_GI_VOLUME_SETS_UPDATE_RATE
                    out.update_rate = rate
                case .diffuseIntensity(let scale):
                    out.sets |= CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY
                    out.diffuse_intensity = scale
                case .specularIntensity(let scale):
                    out.sets |= CY_GI_VOLUME_SETS_SPECULAR_INTENSITY
                    out.specular_intensity = scale
                case .tier(let tier):
                    out.sets |= CY_GI_VOLUME_SETS_TIER
                    out.tier = tier.rawValue
                case .farField(let metres):
                    out.sets |= CY_GI_VOLUME_SETS_FAR_FIELD
                    out.far_field_metres = metres
                }
            }
            return out
        }
    }

    /// A volume the engine already holds.
    public init(handle: CyGiVolume) {
        self.handle = handle
    }

    /// Place a volume. Throws `.invalidArgument` for an empty box, a weight outside 0 to 1 or an
    /// override out of its range.
    public static func place(_ description: Description) throws -> GIVolume {
        var raw = description.raw
        var handle: CyGiVolume = 0
        try GameServices.engine().giVolumeCreate(desc: &raw, into: &handle)
        return GIVolume(handle: handle)
    }

    /// Replace what the volume is: move it, resize it, change what it overrides.
    public func update(_ description: Description) throws {
        var raw = description.raw
        try GameServices.engine().giVolumeUpdate(volume: handle, desc: &raw)
    }

    /// Remove it; its region goes back to the global settings.
    public func remove() throws {
        try GameServices.engine().giVolumeDestroy(volume: handle)
    }
}
