// SPDX-License-Identifier: MIT
// SceneTree.swift — ABI 1.5's `node_find`, as a game writes it. `add-swift-m12-gaps`.
//
//     let camera = SceneTree.find("../Camera", from: entity)     // relative to a node
//     let player = SceneTree.find("/Level/Player")               // absolute, from the root
//
// Callable in every phase: the tree's shape is simulation state and names are unique among
// siblings, so a path names one node or none on every run. Most behaviours never call this directly
// — `@Node(path)` is resolved through it at `onReady`.

import CyberdyneABI
import CyberdyneCore

/// The engine's scene tree, as paths.
public enum SceneTree {
    /// The node `path` names — relative to `node`, or absolute when it starts with `/` — or nil when
    /// it does not resolve, when no scene is bound, or when no engine is.
    public static func find(_ path: String, from node: Entity = .null) -> Entity? {
        guard let engine = try? GameServices.engine(), !path.isEmpty else { return nil }
        var found = CY_ENTITY_NULL
        let resolved: Bool = path.withCString { text in
            (try? engine.nodeFind(from: node.bits, path: text, into: &found)) != nil
        }
        return resolved ? Entity(bits: found) : nil
    }
}
