// SPDX-License-Identifier: MIT
// TreeCallbackTests.swift — ABI 1.5's tree callbacks and `@Node` resolution, through the vtable a
// registration hands the engine. `add-swift-m12-gaps`.
//
// The engine side — that the scene tree's pump calls these in the tree's order — is
// integration.game_backend_scene and integration.rts_api_sample. This proves the Swift half: each
// tree entry is registered only for a class that wrote the callback (or, for `ready`, declares an
// `@Node`), each reaches the right method, `ready` resolves every `@Node` through `node_find` BEFORE
// `onReady` runs, an unresolvable path is nil rather than a trap, and a throwing callback disables
// the instance.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

private enum Seen {
    nonisolated(unsafe) static var vtable = CyBehaviourVTable()
    nonisolated(unsafe) static var paths: [String] = []
    nonisolated(unsafe) static var froms: [CyEntity] = []
}

@Behaviour
final class TreeProbe: Behaviour {
    @Node("../Camera") var camera: Entity?
    @Node("/Level/Missing") var missing: Entity?
    var calls: [String] = []
    var cameraAtReady: Entity?

    override func onEnterTree() { calls.append("enter") }
    override func onReady() {
        calls.append("ready")
        cameraAtReady = camera
    }
    override func onEnable() { calls.append("enable") }
    override func onDisable() throws {
        calls.append("disable")
        throw TreeProbeError.refused
    }
    override func onExitTree() { calls.append("exit") }
}

private enum TreeProbeError: Error { case refused }

/// Only a node reference and no tree callback: `ready` is still registered, to resolve it.
@Behaviour
final class NodeOnly: Behaviour {
    @Node("Rig") var rig: Entity?
}

@Behaviour
final class NoTree: Behaviour {
    override func onFixedUpdate(_ delta: Double) {}
}

final class TreeCallbackTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.paths = []
        Seen.froms = []
        FakeEngine.install { table in
            table.register_behaviour = { _, _, vtable in
                Seen.vtable = vtable?.pointee ?? CyBehaviourVTable()
                return OpaquePointer(bitPattern: 0xB0)
            }
            table.node_find = { _, from, path, out in
                let text = String(cString: path!)
                Seen.paths.append(text)
                Seen.froms.append(from)
                guard text == "../Camera" || text == "Rig" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no node at that path")
                }
                out!.pointee = 77
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    private func create(_ vtable: CyBehaviourVTable, entity: CyEntity = 5) throws -> CyInstance {
        try XCTUnwrap(vtable.create?(FakeEngine.handle, entity, vtable.user_data))
    }

    private func object<T: Behaviour>(_ raw: CyInstance, as type: T.Type) -> T? {
        Unmanaged<Behaviour>.fromOpaque(UnsafeRawPointer(raw)).takeUnretainedValue() as? T
    }

    func testOnlyTheCallbacksAClassWroteAreRegistered() throws {
        try Behaviours.register(NoTree.self)
        XCTAssertNil(Seen.vtable.enter_tree)
        XCTAssertNil(Seen.vtable.ready)
        XCTAssertNil(Seen.vtable.enable)
        XCTAssertNil(Seen.vtable.disable)
        XCTAssertNil(Seen.vtable.exit_tree)

        try Behaviours.register(TreeProbe.self)
        XCTAssertNotNil(Seen.vtable.enter_tree)
        XCTAssertNotNil(Seen.vtable.ready)
        XCTAssertNotNil(Seen.vtable.enable)
        XCTAssertNotNil(Seen.vtable.disable)
        XCTAssertNotNil(Seen.vtable.exit_tree)

        // `@Node` alone is reason enough for `ready`, and for nothing else.
        try Behaviours.register(NodeOnly.self)
        XCTAssertNotNil(Seen.vtable.ready)
        XCTAssertNil(Seen.vtable.enter_tree)
        XCTAssertEqual(NodeOnly.nodePaths, ["Rig"])
        XCTAssertEqual(NoTree.nodePaths, [])
    }

    func testReadyResolvesEveryNodeBeforeOnReadyAndAMissingPathIsNil() throws {
        try Behaviours.register(TreeProbe.self)
        let vtable = Seen.vtable
        let raw = try create(vtable, entity: 5)
        let probe = try XCTUnwrap(object(raw, as: TreeProbe.self))

        vtable.enter_tree?(raw, vtable.user_data)
        vtable.ready?(raw, vtable.user_data)
        XCTAssertEqual(probe.calls, ["enter", "ready"])
        // Resolved from the behaviour's own node, before `onReady` read it.
        XCTAssertEqual(probe.cameraAtReady, Entity(bits: 77))
        XCTAssertEqual(probe.camera, Entity(bits: 77))
        XCTAssertNil(probe.missing, "a path that does not resolve is nil, not a trap")
        XCTAssertEqual(Seen.paths, ["../Camera", "/Level/Missing"])
        XCTAssertEqual(Seen.froms, [5, 5])
        vtable.destroy?(raw, vtable.user_data)
    }

    func testEachTreeEntryReachesItsMethodAndAThrowDisablesTheInstance() throws {
        try Behaviours.register(TreeProbe.self)
        let vtable = Seen.vtable
        let raw = try create(vtable)
        let probe = try XCTUnwrap(object(raw, as: TreeProbe.self))

        vtable.enable?(raw, vtable.user_data)
        vtable.disable?(raw, vtable.user_data)  // throws: the instance is disabled
        XCTAssertFalse(probe.isEnabled)
        vtable.exit_tree?(raw, vtable.user_data)  // not delivered to a disabled instance
        XCTAssertEqual(probe.calls, ["enable", "disable"])
        vtable.destroy?(raw, vtable.user_data)
    }

    func testSceneTreeFindIsNilWhenAPathDoesNotResolve() {
        XCTAssertEqual(SceneTree.find("../Camera", from: Entity(bits: 3)), Entity(bits: 77))
        XCTAssertNil(SceneTree.find("Nowhere", from: Entity(bits: 3)))
        XCTAssertNil(SceneTree.find(""))
        FakeEngine.uninstall()
        XCTAssertNil(SceneTree.find("../Camera"), "no engine is nil too, never a trap")
    }
}
