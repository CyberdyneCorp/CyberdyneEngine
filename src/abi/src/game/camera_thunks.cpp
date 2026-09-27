// SPDX-License-Identifier: MIT
// The `camera` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
// Each thunk takes cy/abi/game/services.h's steps in order — engine, phase, backend, pointers,
// `struct_size`, the resimulation drop — and only then asks the `CameraBackend`.
//
// PHASES. The camera is presentation. Its reads (`camera_active`, `camera_view` and the two
// projections) are `[N U]`: a fixed step that branched on the camera would not replay on a peer
// with a different window. Its writes are `[N F U]`, because nothing in the simulation reads them
// back, and while the clock says the tick is a resimulation they answer OK and do nothing — a
// rolled-back tick must not move the camera a second time.

#include <cy/abi/errors.h>
#include <cy/abi/game/camera.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

/// Presentation reads: a frame update or no phase, never a fixed step.
constexpr u32 kReadPhases = kPhaseNone | kPhaseFrame;

/// Steps 1 to 3: the engine, the phase, the backend. Null, with `result` set, when any refuses —
/// returned as the pointer rather than through an out-parameter so that every use after the check
/// is visibly non-null.
[[nodiscard]] CameraBackend* enter(CyEngine engine, u32 phases, const char* entry,
                                   CyResult& result) noexcept {
    result = CY_RESULT_OK;
    if (engine == nullptr) {
        result = report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    if (const CyResult allowed = require_phase(engine->game, phases, entry);
        allowed != CY_RESULT_OK) {
        result = allowed;
        return nullptr;
    }
    if (engine->game.camera == nullptr) {
        result = report(CY_RESULT_UNAVAILABLE, "no camera backend is bound to this engine");
    }
    return engine->game.camera;
}

[[nodiscard]] CyResult invalid(const char* what) noexcept {
    return report(CY_RESULT_INVALID_ARGUMENT, what);
}

/// The backend's answer, with the last error cleared on success.
[[nodiscard]] CyResult finish(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// Step 5: a presentation write while resimulating is accepted and dropped.
[[nodiscard]] bool dropped(CyEngine engine) noexcept {
    if (!engine->game.clock.resimulating()) {
        return false;
    }
    clear_last_error();
    return true;
}

}  // namespace

CyResult camera_active(CyEngine engine, CyCamera* out_camera) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kReadPhases, "camera_active", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (out_camera == nullptr) {
        return invalid("camera_active needs an output");
    }
    CyCamera camera = CY_CAMERA_NULL;
    if (const CyResult result = backend->active(camera); result != CY_RESULT_OK) {
        return result;
    }
    *out_camera = camera;
    return finish(CY_RESULT_OK);
}

CyResult camera_view(CyEngine engine, CyCamera camera, CyCameraView* out_view) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kReadPhases, "camera_view", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (out_view == nullptr) {
        return invalid("camera_view needs an output");
    }
    if (agreed_size<CyCameraView>(out_view->struct_size) == 0U) {
        return invalid("camera_view: malformed struct_size");
    }
    CyCameraView view{};
    view.struct_size = sizeof(CyCameraView);
    if (const CyResult result = backend->view(camera, view); result != CY_RESULT_OK) {
        return result;
    }
    (void)write_sized(*out_view, view);
    return finish(CY_RESULT_OK);
}

CyResult camera_screen_to_ray(CyEngine engine, CyCamera camera, const float* screen_xy,
                              CyRay* out_ray) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kReadPhases, "camera_screen_to_ray", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (screen_xy == nullptr || out_ray == nullptr) {
        return invalid("camera_screen_to_ray needs a screen point and an output");
    }
    if (!std::isfinite(screen_xy[0]) || !std::isfinite(screen_xy[1])) {
        return invalid("camera_screen_to_ray: the screen point is not finite");
    }
    CyRay ray{};
    if (const CyResult result = backend->screen_to_ray(camera, screen_xy[0], screen_xy[1], ray);
        result != CY_RESULT_OK) {
        return result;
    }
    *out_ray = ray;
    return finish(CY_RESULT_OK);
}

CyResult camera_world_to_screen(CyEngine engine, CyCamera camera, const float* points_xyz,
                                uint32_t count, CyScreenPoint* out_points) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kReadPhases, "camera_world_to_screen", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (count != 0U && (points_xyz == nullptr || out_points == nullptr)) {
        return invalid("camera_world_to_screen needs points and room for as many results");
    }
    // Zero points still reach the backend, so a stale camera is NOT_FOUND whatever the count.
    const Span<const f32> points(points_xyz, static_cast<usize>(count) * 3U);
    const Span<CyScreenPoint> results(out_points, count);
    return finish(backend->world_to_screen(camera, points, results));
}

CyResult camera_set_target(CyEngine engine, CyCamera camera, const CyCameraTarget* target) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kPhaseAny, "camera_set_target", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (target == nullptr) {
        return invalid("camera_set_target needs a target");
    }
    CyCameraTarget whole{};
    if (!read_sized(*target, whole)) {
        return invalid("camera_set_target: malformed struct_size");
    }
    if (dropped(engine)) {
        return CY_RESULT_OK;
    }
    return finish(backend->set_target(camera, whole));
}

CyResult camera_set_pose(CyEngine engine, CyCamera camera, const CyPose* pose) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kPhaseAny, "camera_set_pose", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (pose == nullptr) {
        return invalid("camera_set_pose needs a pose");
    }
    if (dropped(engine)) {
        return CY_RESULT_OK;
    }
    return finish(backend->set_pose(camera, *pose));
}

CyResult camera_clear_pose(CyEngine engine, CyCamera camera) {
    CyResult entered = CY_RESULT_OK;
    CameraBackend* backend = enter(engine, kPhaseAny, "camera_clear_pose", entered);
    if (backend == nullptr) {
        return entered;
    }
    if (dropped(engine)) {
        return CY_RESULT_OK;
    }
    return finish(backend->clear_pose(camera));
}

}  // namespace cy::abi::game
