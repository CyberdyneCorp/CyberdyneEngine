// SPDX-License-Identifier: MIT
// ABI 1.3's `camera_*` entries, against a fake `CameraBackend`. `add-swift-game-api`.
//
// OWNER: implementer A. The boundary only: which phases each entry answers in, what reaches the
// backend, `struct_size` both ways, and the resimulation drop. The projections themselves are the
// adapter's, proven against a real camera server in src/game_backend/tests/test_camera_backend.cpp.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/camera.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyCamera kCamera = 0x100000003ULL;

class FakeCamera final : public cy::abi::game::CameraBackend {
public:
    CyResult answer = CY_RESULT_OK;
    int calls = 0;
    CyCamera last_camera = CY_CAMERA_NULL;
    float last_screen[2] = {0.0F, 0.0F};
    std::size_t last_points = 0;
    std::size_t last_results = 0;
    CyCameraTarget last_target{};
    CyPose last_pose{};
    cy::u32 seen_view_size = 0;

    CyResult active(CyCamera& out_camera) noexcept override {
        ++calls;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: no primary view");
        }
        out_camera = kCamera;
        return CY_RESULT_OK;
    }
    CyResult view(CyCamera camera, CyCameraView& out_view) noexcept override {
        ++calls;
        last_camera = camera;
        seen_view_size = out_view.struct_size;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: refused");
        }
        out_view.vertical_fov = 1.0F;
        out_view.near_plane = 0.1F;
        out_view.far_plane = 500.0F;
        out_view.viewport[2] = 1280.0F;
        return CY_RESULT_OK;
    }
    CyResult screen_to_ray(CyCamera camera, cy::f32 screen_x, cy::f32 screen_y,
                           CyRay& out_ray) noexcept override {
        ++calls;
        last_camera = camera;
        last_screen[0] = screen_x;
        last_screen[1] = screen_y;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: refused");
        }
        out_ray.direction[2] = -1.0F;
        out_ray.max_distance = 499.9F;
        return CY_RESULT_OK;
    }
    CyResult world_to_screen(CyCamera camera, cy::Span<const cy::f32> points_xyz,
                             cy::Span<CyScreenPoint> out_points) noexcept override {
        ++calls;
        last_camera = camera;
        last_points = points_xyz.size();
        last_results = out_points.size();
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: refused");
        }
        for (CyScreenPoint& point : out_points) {
            point.flags = CY_SCREEN_POINT_ON_SCREEN;
        }
        return CY_RESULT_OK;
    }
    CyResult set_target(CyCamera camera, const CyCameraTarget& target) noexcept override {
        ++calls;
        last_camera = camera;
        last_target = target;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
    CyResult set_pose(CyCamera camera, const CyPose& pose) noexcept override {
        ++calls;
        last_camera = camera;
        last_pose = pose;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
    CyResult clear_pose(CyCamera camera) noexcept override {
        ++calls;
        last_camera = camera;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
};

struct Bench {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeCamera camera;

    explicit Bench(CyPhase phase = CY_PHASE_NONE) noexcept {
        host.game.camera = &camera;
        host.game.clock.phase = phase;
        cy::abi::clear_last_error();
    }
    CyEngine engine() noexcept { return &host; }
};

CyCameraTarget a_target() noexcept {
    CyCameraTarget target{};
    target.struct_size = sizeof(CyCameraTarget);
    target.position[0] = 10.0F;
    target.yaw = 0.5F;
    target.pitch = -0.8F;
    target.distance = 40.0F;
    target.blend_seconds = 0.25F;
    return target;
}

}  // namespace

CY_TEST_CASE("camera: the reads are presentation state, refused in a fixed step") {
    Bench bench(CY_PHASE_FIXED_UPDATE);
    CyCamera camera = 77;
    CyCameraView view{};
    const float screen[2] = {1.0F, 2.0F};
    CyRay ray{};
    const float point[3] = {0.0F, 0.0F, 0.0F};
    CyScreenPoint projected{};
    CY_CHECK_EQ(table().camera_active(bench.engine(), &camera), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "camera_active") != nullptr);
    CY_CHECK_EQ(table().camera_view(bench.engine(), kCamera, &view), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(table().camera_screen_to_ray(bench.engine(), kCamera, screen, &ray),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(table().camera_world_to_screen(bench.engine(), kCamera, point, 1, &projected),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(bench.camera.calls, 0);
    CY_CHECK_EQ(camera, 77U);
}

CY_TEST_CASE("camera: active and view answer in no phase and in a frame update") {
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FRAME_UPDATE}) {
        Bench bench(phase);
        CyCamera camera = CY_CAMERA_NULL;
        CY_CHECK_EQ(table().camera_active(bench.engine(), &camera), CY_RESULT_OK);
        CY_CHECK_EQ(camera, kCamera);
        CyCameraView view{};
        CY_CHECK_EQ(table().camera_view(bench.engine(), camera, &view), CY_RESULT_OK);
        CY_CHECK_EQ(bench.camera.last_camera, kCamera);
        CY_CHECK_EQ(bench.camera.seen_view_size, sizeof(CyCameraView));
        CY_CHECK_EQ(view.struct_size, sizeof(CyCameraView));
        CY_CHECK_EQ(view.far_plane, 500.0F);
        CY_CHECK_EQ(view.viewport[2], 1280.0F);
    }
}

CY_TEST_CASE("camera: no primary view is UNAVAILABLE and leaves the output alone") {
    Bench bench;
    bench.camera.answer = CY_RESULT_UNAVAILABLE;
    CyCamera camera = 5;
    CY_CHECK_EQ(table().camera_active(bench.engine(), &camera), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(camera, 5U);
    bench.camera.answer = CY_RESULT_NOT_FOUND;
    CyCameraView view{};
    view.near_plane = 3.0F;
    CY_CHECK_EQ(table().camera_view(bench.engine(), 1, &view), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(view.near_plane, 3.0F);
}

CY_TEST_CASE("camera: a view caller compiled against a shorter struct gets only its prefix") {
    Bench bench;
    CyCameraView view{};
    std::memset(&view, 0xEE, sizeof(view));
    const auto prefix = static_cast<cy::u32>(offsetof(CyCameraView, far_plane));
    view.struct_size = prefix;
    CY_CHECK_EQ(table().camera_view(bench.engine(), kCamera, &view), CY_RESULT_OK);
    CY_CHECK_EQ(view.struct_size, prefix);
    CY_CHECK_EQ(view.near_plane, 0.1F);
    cy::u32 untouched = 0;
    std::memcpy(&untouched, &view.far_plane, sizeof(untouched));
    CY_CHECK_EQ(untouched, 0xEEEEEEEEU);

    view.struct_size = 3;
    const int calls = bench.camera.calls;
    CY_CHECK_EQ(table().camera_view(bench.engine(), kCamera, &view), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bench.camera.calls, calls);
}

CY_TEST_CASE("camera: screen_to_ray passes the point and refuses a missing or non-finite one") {
    Bench bench(CY_PHASE_FRAME_UPDATE);
    const float screen[2] = {320.0F, 200.0F};
    CyRay ray{};
    CY_CHECK_EQ(table().camera_screen_to_ray(bench.engine(), kCamera, screen, &ray), CY_RESULT_OK);
    CY_CHECK_EQ(bench.camera.last_screen[0], 320.0F);
    CY_CHECK_EQ(bench.camera.last_screen[1], 200.0F);
    CY_CHECK_EQ(ray.max_distance, 499.9F);

    CY_CHECK_EQ(table().camera_screen_to_ray(bench.engine(), kCamera, nullptr, &ray),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().camera_screen_to_ray(bench.engine(), kCamera, screen, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    const float nan_point[2] = {std::numeric_limits<float>::quiet_NaN(), 0.0F};
    CY_CHECK_EQ(table().camera_screen_to_ray(bench.engine(), kCamera, nan_point, &ray),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bench.camera.calls, 1);
}

CY_TEST_CASE("camera: world_to_screen hands the backend three floats per point") {
    Bench bench(CY_PHASE_FRAME_UPDATE);
    const float points[6] = {0.0F, 0.0F, -5.0F, 1.0F, 2.0F, -9.0F};
    CyScreenPoint projected[2] = {};
    CY_CHECK_EQ(table().camera_world_to_screen(bench.engine(), kCamera, points, 2, projected),
                CY_RESULT_OK);
    CY_CHECK_EQ(bench.camera.last_points, 6U);
    CY_CHECK_EQ(bench.camera.last_results, 2U);
    CY_CHECK_EQ(projected[1].flags, CY_SCREEN_POINT_ON_SCREEN);

    CY_CHECK_EQ(table().camera_world_to_screen(bench.engine(), kCamera, nullptr, 2, projected),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().camera_world_to_screen(bench.engine(), kCamera, points, 2, nullptr),
                CY_RESULT_INVALID_ARGUMENT);

    // No points still asks the backend, so a stale camera is NOT_FOUND whatever the count.
    bench.camera.answer = CY_RESULT_NOT_FOUND;
    CY_CHECK_EQ(table().camera_world_to_screen(bench.engine(), 9, nullptr, 0, nullptr),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(bench.camera.last_points, 0U);
}

CY_TEST_CASE("camera: the writes are allowed in every phase, a fixed step included") {
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        Bench bench(phase);
        const CyCameraTarget target = a_target();
        CY_CHECK_EQ(table().camera_set_target(bench.engine(), kCamera, &target), CY_RESULT_OK);
        CY_CHECK_EQ(bench.camera.last_target.distance, 40.0F);
        CY_CHECK_EQ(bench.camera.last_target.struct_size, sizeof(CyCameraTarget));
        CyPose pose{};
        pose.position[1] = 12.0F;
        CY_CHECK_EQ(table().camera_set_pose(bench.engine(), kCamera, &pose), CY_RESULT_OK);
        CY_CHECK_EQ(bench.camera.last_pose.position[1], 12.0F);
        CY_CHECK_EQ(table().camera_clear_pose(bench.engine(), kCamera), CY_RESULT_OK);
        CY_CHECK_EQ(bench.camera.calls, 3);
    }
}

CY_TEST_CASE("camera: a write while resimulating succeeds and does nothing") {
    Bench bench(CY_PHASE_FIXED_UPDATE);
    bench.host.game.clock.flags = CY_TIME_RESIMULATING;
    const CyCameraTarget target = a_target();
    CyPose pose{};
    CY_CHECK_EQ(table().camera_set_target(bench.engine(), kCamera, &target), CY_RESULT_OK);
    CY_CHECK_EQ(table().camera_set_pose(bench.engine(), kCamera, &pose), CY_RESULT_OK);
    CY_CHECK_EQ(table().camera_clear_pose(bench.engine(), kCamera), CY_RESULT_OK);
    CY_CHECK_EQ(bench.camera.calls, 0);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OK);

    // The arguments are still checked: a resimulated tick is not a licence to pass garbage.
    CY_CHECK_EQ(table().camera_set_target(bench.engine(), kCamera, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().camera_set_pose(bench.engine(), kCamera, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("camera: a target from an older caller is read to its size and zero after it") {
    Bench bench;
    CyCameraTarget target = a_target();
    target.struct_size = static_cast<cy::u32>(offsetof(CyCameraTarget, distance));
    CY_CHECK_EQ(table().camera_set_target(bench.engine(), kCamera, &target), CY_RESULT_OK);
    CY_CHECK_EQ(bench.camera.last_target.pitch, -0.8F);
    CY_CHECK_EQ(bench.camera.last_target.distance, 0.0F);
    CY_CHECK_EQ(bench.camera.last_target.blend_seconds, 0.0F);
    CY_CHECK_EQ(bench.camera.last_target.struct_size, sizeof(CyCameraTarget));

    target.struct_size = 1;
    CY_CHECK_EQ(table().camera_set_target(bench.engine(), kCamera, &target),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bench.camera.calls, 1);
}

CY_TEST_CASE("camera: every entry refuses a null engine and a missing backend") {
    const CyInterface& iface = table();
    CyCamera camera = 0;
    CyPose pose{};
    CY_CHECK_EQ(iface.camera_active(nullptr, &camera), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.camera_clear_pose(nullptr, kCamera), CY_RESULT_INVALID_ARGUMENT);

    Bench bench;
    CY_CHECK_EQ(iface.camera_active(bench.engine(), nullptr), CY_RESULT_INVALID_ARGUMENT);
    bench.host.game.camera = nullptr;
    CY_CHECK_EQ(iface.camera_active(bench.engine(), &camera), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.camera_set_pose(bench.engine(), kCamera, &pose), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.camera_clear_pose(bench.engine(), kCamera), CY_RESULT_UNAVAILABLE);
    const CyCameraTarget target = a_target();
    CY_CHECK_EQ(iface.camera_set_target(bench.engine(), kCamera, &target), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("camera: a write the backend refuses comes back with its code") {
    Bench bench;
    bench.camera.answer = CY_RESULT_NOT_FOUND;
    const CyCameraTarget target = a_target();
    CY_CHECK_EQ(table().camera_set_target(bench.engine(), 1, &target), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(table().camera_clear_pose(bench.engine(), 1), CY_RESULT_NOT_FOUND);
}
