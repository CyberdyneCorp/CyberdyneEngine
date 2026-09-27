// SPDX-License-Identifier: MIT
// CameraTests.swift — the `Camera` facade through `FakeEngine`. `add-swift-game-api`.
//
// OWNER: implementer A. Each case installs the entries it exercises, calls the facade, and checks
// what the entry received, what the facade made of the answer, and how a refusal surfaces.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries were last asked. C function pointers cannot capture, so they write here.
private enum CameraSeen {
    nonisolated(unsafe) static var camera: CyCamera = 0
    nonisolated(unsafe) static var screen: (Float, Float) = (0, 0)
    nonisolated(unsafe) static var points: [Float] = []
    nonisolated(unsafe) static var target = CyCameraTarget()
    nonisolated(unsafe) static var pose = CyPose()
    nonisolated(unsafe) static var cleared = false

    static func reset() {
        camera = 0
        screen = (0, 0)
        points = []
        target = CyCameraTarget()
        pose = CyPose()
        cleared = false
    }
}

final class CameraTests: XCTestCase {
    override func setUp() {
        super.setUp()
        CameraSeen.reset()
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testTheActiveCameraIsItsHandle() throws {
        FakeEngine.install { table in
            table.camera_active = { _, camera in
                camera!.pointee = 0x1_0000_0007
                return CY_RESULT_OK
            }
        }
        XCTAssertEqual(try Camera.active(), Camera(raw: 0x1_0000_0007))
    }

    func testNoPrimaryViewIsNilButARefusalStillThrows() {
        FakeEngine.install { table in
            table.camera_active = { _, _ in
                FakeEngine.fail(CY_RESULT_UNAVAILABLE, "camera: there is no primary view")
            }
        }
        XCTAssertNil(try Camera.active())

        FakeEngine.install { table in
            table.camera_active = { _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "camera_active may not be called during fixed update")
            }
        }
        XCTAssertThrowsError(try Camera.active()) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(
                    .permissionDenied,
                    message: "camera_active may not be called during fixed update"))
        }
    }

    func testAPickRayGoesThroughThePointer() throws {
        FakeEngine.install { table in
            table.camera_screen_to_ray = { _, camera, screen, ray in
                CameraSeen.camera = camera
                CameraSeen.screen = (screen![0], screen![1])
                ray!.pointee.origin = (1, 2, 3)
                ray!.pointee.direction = (0, 0, -1)
                ray!.pointee.max_distance = 999
                return CY_RESULT_OK
            }
            table.input_pointer = { _, _, pointer in
                pointer!.pointee.flags = CY_INPUT_POINTER_PRESENT
                pointer!.pointee.position = (320, 240)
                return CY_RESULT_OK
            }
        }
        let camera = Camera(raw: 5)
        let ray = try camera.ray(under: Input.pointer())
        XCTAssertEqual(CameraSeen.camera, 5)
        XCTAssertEqual(CameraSeen.screen.0, 320)
        XCTAssertEqual(CameraSeen.screen.1, 240)
        XCTAssertEqual(
            ray,
            Ray(
                origin: Vec3(x: 1, y: 2, z: 3), direction: Vec3(x: 0, y: 0, z: -1), maxDistance: 999
            ))
    }

    func testProjectingSendsThreeFloatsPerPointAndKeepsTheOrder() throws {
        FakeEngine.install { table in
            table.camera_world_to_screen = { _, camera, points, count, out in
                CameraSeen.camera = camera
                CameraSeen.points = Array(UnsafeBufferPointer(start: points, count: Int(count) * 3))
                for index in 0..<Int(count) {
                    out![index].position = (Float(index) * 10, 5)
                    out![index].depth = Float(index) - 0.5
                    out![index].flags =
                        index == 0 ? CY_SCREEN_POINT_BEHIND : CY_SCREEN_POINT_ON_SCREEN
                }
                return CY_RESULT_OK
            }
        }
        let camera = Camera(raw: 8)
        let points = try camera.project([Vec3(x: 1, y: 2, z: 3), Vec3(x: 4, y: 5, z: 6)])
        XCTAssertEqual(CameraSeen.points, [1, 2, 3, 4, 5, 6])
        XCTAssertEqual(points.count, 2)
        XCTAssertTrue(points[0].isBehind)
        XCTAssertFalse(points[0].isOnScreen)
        XCTAssertTrue(points[1].isOnScreen)
        XCTAssertEqual(points[1].position, Vec2(x: 10, y: 5))
        XCTAssertEqual(points[1].depth, 0.5)
        XCTAssertEqual(try camera.project(Vec3(x: 7, y: 8, z: 9)).position, Vec2(x: 0, y: 5))
        XCTAssertEqual(try camera.project([]), [])
    }

    func testTheViewConvertsPoseProjectionAndViewport() throws {
        FakeEngine.install { table in
            table.camera_view = { _, camera, view in
                CameraSeen.camera = camera
                XCTAssertEqual(view!.pointee.struct_size, UInt32(MemoryLayout<CyCameraView>.size))
                view!.pointee.flags = CY_CAMERA_VIEW_ORTHOGRAPHIC
                view!.pointee.pose.position = (0, 30, 10)
                view!.pointee.ortho_height = 50
                view!.pointee.near_plane = 0.1
                view!.pointee.far_plane = 0
                view!.pointee.viewport = (100, 50, 1280, 720)
                return CY_RESULT_OK
            }
        }
        let view = try Camera(raw: 2).view()
        XCTAssertEqual(CameraSeen.camera, 2)
        XCTAssertTrue(view.isOrthographic)
        XCTAssertEqual(view.orthographicHeight, 50)
        XCTAssertEqual(view.pose.position, Vec3(x: 0, y: 30, z: 10))
        XCTAssertEqual(view.viewport, Vec4(x: 100, y: 50, z: 1280, w: 720))
        XCTAssertEqual(view.farPlane, 0)
    }

    func testATargetCarriesItsFocusAngleAndDistance() throws {
        FakeEngine.install { table in
            table.camera_set_target = { _, camera, target in
                CameraSeen.camera = camera
                CameraSeen.target = target!.pointee
                return CY_RESULT_OK
            }
        }
        let camera = Camera(raw: 4)
        try camera.setTarget(
            CameraTarget(
                focus: .position(Vec3(x: 10, y: 0, z: -4)), yaw: 0.8, pitch: -0.9, distance: 40,
                blendSeconds: 0.5))
        XCTAssertEqual(CameraSeen.camera, 4)
        XCTAssertEqual(CameraSeen.target.struct_size, UInt32(MemoryLayout<CyCameraTarget>.size))
        XCTAssertEqual(CameraSeen.target.flags, 0)
        XCTAssertEqual(CameraSeen.target.position.0, 10)
        XCTAssertEqual(CameraSeen.target.position.2, -4)
        XCTAssertEqual(CameraSeen.target.yaw, 0.8)
        XCTAssertEqual(CameraSeen.target.pitch, -0.9)
        XCTAssertEqual(CameraSeen.target.distance, 40)
        XCTAssertEqual(CameraSeen.target.blend_seconds, 0.5)

        try camera.setTarget(CameraTarget(focus: .entity(0x2_0000_0011), distance: 12))
        XCTAssertEqual(CameraSeen.target.flags, CY_CAMERA_TARGET_FOLLOW_ENTITY)
        XCTAssertEqual(CameraSeen.target.entity, 0x2_0000_0011)
    }

    func testAPoseOverrideAndItsRelease() throws {
        FakeEngine.install { table in
            table.camera_set_pose = { _, camera, pose in
                CameraSeen.camera = camera
                CameraSeen.pose = pose!.pointee
                return CY_RESULT_OK
            }
            table.camera_clear_pose = { _, camera in
                CameraSeen.camera = camera
                CameraSeen.cleared = true
                return FakeEngine.fail(CY_RESULT_NOT_FOUND, "camera: no live camera rig")
            }
        }
        let camera = Camera(raw: 6)
        try camera.overridePose(Pose(position: Vec3(x: 1, y: 50, z: 2)))
        XCTAssertEqual(CameraSeen.pose.position.1, 50)
        XCTAssertEqual(CameraSeen.pose.rotation.3, 1)
        XCTAssertThrowsError(try camera.clearPoseOverride()) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.notFound, message: "camera: no live camera rig"))
        }
        XCTAssertTrue(CameraSeen.cleared)
    }
}
