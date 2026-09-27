// SPDX-License-Identifier: MIT
// Spawn.swift — ABI 1.3's `spawn_*` entries, as a game writes them. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See openspec/changes/add-swift-game-api/design.md.
//
//     let tank = try World.spawn(prefab: "units/tank", at: Pose(position: p))
//     let squad = try Spawn.prefab("units/rifleman").instantiate(at: formation, parent: tank)
//     try World.destroy(tank)                        // the whole subtree, children first
//
// PHASES. Creating and destroying entities is simulation, so `instantiate` and `destroy` are
// allowed in a fixed step (`onFixedUpdate`) and at initialisation, and REFUSED in a frame update
// (`onUpdate`) with `CyberdyneError.status(.permissionDenied, …)`: a spawn made from a frame would
// exist in one run and not in its replay. The RTS shape is: decide in `onUpdate`, record an order,
// spawn in `onFixedUpdate`.
//
// RESIDENCY. `Spawn.prefab(_:)` may load the asset only at initialisation (no phase). In a fixed
// step or a frame an asset that is not resident throws `.unavailable` — resolve every prefab a
// behaviour will spawn in `onCreate` or module initialisation, keep the `Prefab`, and spawning in
// the fixed step never stalls on a load.
//
// DETERMINISM. The same calls in the same fixed step produce the same entities on every run.

import CyberdyneABI
import CyberdyneCore

/// A resolved prefab or scene asset.
public struct Prefab: Hashable, Sendable {
    /// The engine's handle.
    public let raw: CyPrefab

    /// Wrap a handle the engine handed out.
    public init(raw: CyPrefab) {
        self.raw = raw
    }

    /// Instantiate one copy under `parent` (the level's root when null) at `pose`, scaled by
    /// `scale`, and return its root entity. Every node of it exists, with its behaviours created,
    /// when this returns.
    @discardableResult
    public func instantiate(
        at pose: Pose = Pose(), scale: Vec3 = Vec3(x: 1, y: 1, z: 1), parent: Entity = .null
    ) throws -> Entity {
        var params = CySpawnParams()
        params.struct_size = UInt32(MemoryLayout<CySpawnParams>.size)
        params.parent = parent.bits
        params.pose = pose.raw
        params.scale = scale.tuple
        var root = CY_ENTITY_NULL
        try GameServices.engine().spawnInstantiate(prefab: raw, params: &params, root: &root)
        return Entity(bits: root)
    }

    /// One copy per pose under `parent`, created as one batch: all of them, or — when any fails —
    /// none. The roots are returned in pose order.
    @discardableResult
    public func instantiate(at poses: [Pose], parent: Entity = .null) throws -> [Entity] {
        guard !poses.isEmpty else { return [] }
        let engine = try GameServices.engine()
        let raw = poses.map(\.raw)
        var roots = [CyEntity](repeating: CY_ENTITY_NULL, count: poses.count)
        try raw.withUnsafeBufferPointer { poses in
            try roots.withUnsafeMutableBufferPointer { roots in
                try engine.spawnInstantiateMany(
                    prefab: self.raw, parent: parent.bits, poses: poses.baseAddress,
                    count: UInt32(poses.count), roots: roots.baseAddress)
            }
        }
        return roots.map { Entity(bits: $0) }
    }
}

/// Prefab instantiation. Every call throws the engine's refusal as `CyberdyneError.status`.
public enum Spawn {
    /// Resolve a prefab or scene asset by its content path. `.notFound` for an unknown path;
    /// `.unavailable` outside initialisation for one that is not resident yet.
    public static func prefab(_ path: String) throws -> Prefab {
        let engine = try GameServices.engine()
        var prefab: CyPrefab = 0  // CY_PREFAB_NULL
        try path.withCString { try engine.spawnResolve(asset: $0, into: &prefab) }
        return Prefab(raw: prefab)
    }

    /// Destroy `root` and its whole subtree, children first, running each node's exit and destroy
    /// callbacks. `.notFound` when the entity is not alive.
    public static func destroy(_ root: Entity) throws {
        try GameServices.engine().spawnDestroy(root: root.bits)
    }
}

extension World {
    /// Resolve `prefab` and instantiate one copy of it: `World.spawn(prefab: "units/tank", at: p)`.
    /// A behaviour that spawns the same prefab often keeps `Spawn.prefab(_:)`'s answer instead.
    @discardableResult
    public static func spawn(
        prefab path: String, at pose: Pose = Pose(), scale: Vec3 = Vec3(x: 1, y: 1, z: 1),
        parent: Entity = .null
    ) throws -> Entity {
        try Spawn.prefab(path).instantiate(at: pose, scale: scale, parent: parent)
    }

    /// Destroy `root` and its subtree. The same as `Spawn.destroy(_:)`.
    public static func destroy(_ root: Entity) throws {
        try Spawn.destroy(root)
    }
}
