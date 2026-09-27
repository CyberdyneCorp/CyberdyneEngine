// SPDX-License-Identifier: MIT
// The `physics` thunks of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer B (physics queries and navigation). See
// openspec/changes/add-swift-game-api/design.md.
//
// Each thunk runs cy/abi/game/services.h's steps before the backend sees anything: engine, phase
// (`[N F U]` for all four), backend bound, required pointers, then the argument values. A filter is
// normalised to a whole `CyQueryFilter` — the default one when the caller passed null — so the
// backend never reads a short struct or a null filter. None of the four is structural and none is
// a presentation write, so there is no epoch bump and no resimulation drop.
//
// THE VALUE CHECKS LIVE HERE, NOT IN THE ADAPTER, because a NaN ray or a negative radius is a
// malformed call whatever backend answers it, and the fake backend `unit.abi` binds must see the
// same refusal the real one would.

#include <cy/abi/errors.h>
#include <cy/abi/game/physics.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kPhysicsPhases = kPhaseAny;
constexpr u32 kLayerCount = 32;

/// Steps 1–3: the engine, the phase and the backend. Null `out` means the call was refused.
CyResult enter(CyEngine engine, const char* entry, PhysicsQueryBackend*& out) noexcept {
    out = nullptr;
    if (engine == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "engine is null");
    }
    if (const CyResult phase = require_phase(engine->game, kPhysicsPhases, entry);
        phase != CY_RESULT_OK) {
        return phase;
    }
    if (engine->game.physics == nullptr) {
        return report(CY_RESULT_UNAVAILABLE, "no physics query backend is bound to this engine");
    }
    out = engine->game.physics;
    return CY_RESULT_OK;
}

bool finite3(const float* value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool finite_non_negative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

bool finite_positive(float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

CyResult check_ray(const CyRay& ray) noexcept {
    if (!finite3(ray.origin) || !finite3(ray.direction)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ray origin and direction must be finite");
    }
    if (ray.direction[0] == 0.0F && ray.direction[1] == 0.0F && ray.direction[2] == 0.0F) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ray direction is zero");
    }
    if (!finite_non_negative(ray.max_distance)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ray max_distance must be finite and not negative");
    }
    return CY_RESULT_OK;
}

CyResult check_shape(const CyShape& shape) noexcept {
    switch (shape.kind) {
        case CY_SHAPE_SPHERE:
            if (finite_positive(shape.radius)) {
                return CY_RESULT_OK;
            }
            return report(CY_RESULT_INVALID_ARGUMENT, "a sphere needs a positive, finite radius");
        case CY_SHAPE_CAPSULE:
            if (finite_positive(shape.radius) && finite_non_negative(shape.half_height)) {
                return CY_RESULT_OK;
            }
            return report(CY_RESULT_INVALID_ARGUMENT,
                          "a capsule needs a positive radius and a non-negative half_height");
        case CY_SHAPE_BOX:
            if (finite_positive(shape.half_extents[0]) && finite_positive(shape.half_extents[1]) &&
                finite_positive(shape.half_extents[2])) {
                return CY_RESULT_OK;
            }
            return report(CY_RESULT_INVALID_ARGUMENT, "a box needs positive, finite half_extents");
        default:
            return report(CY_RESULT_INVALID_ARGUMENT, "unknown CyShapeKind");
    }
}

CyResult check_pose(const CyPose& pose) noexcept {
    const bool finite_rotation = std::isfinite(pose.rotation[0]) &&
                                 std::isfinite(pose.rotation[1]) &&
                                 std::isfinite(pose.rotation[2]) && std::isfinite(pose.rotation[3]);
    if (!finite3(pose.position) || !finite_rotation) {
        return report(CY_RESULT_INVALID_ARGUMENT, "pose must be finite");
    }
    return CY_RESULT_OK;
}

/// The filter the backend sees: the caller's, whole, or the documented default for a null one.
CyResult normalise_filter(const CyQueryFilter* in, CyQueryFilter& out) noexcept {
    if (in == nullptr) {
        out = CyQueryFilter{};
        out.struct_size = static_cast<u32>(sizeof(CyQueryFilter));
        out.mask = 0xFFFFFFFFU;
        return CY_RESULT_OK;
    }
    if (!read_sized(*in, out)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "filter has a malformed struct_size");
    }
    if (out.layer >= kLayerCount) {
        return report(CY_RESULT_INVALID_ARGUMENT, "filter layer must be 0 to 31");
    }
    if (out.ignore_count > 0U && out.ignore == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "filter ignore is null with a non-zero count");
    }
    return CY_RESULT_OK;
}

/// The sizing pattern's last step: the total is always reported; a short, non-null buffer is
/// BUFFER_TOO_SMALL having been filled; a null one was a question.
CyResult finish_sized(const void* buffer, uint32_t capacity, u32 total, uint32_t* out_count,
                      const char* what) noexcept {
    *out_count = total;
    if (buffer != nullptr && total > capacity) {
        return report(CY_RESULT_BUFFER_TOO_SMALL, what);
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace

CyResult physics_raycast(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                         CyPhysicsHit* out_hit, bool* out_has_hit) {
    PhysicsQueryBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, "physics_raycast", backend); backend == nullptr) {
        return entered;
    }
    if (ray == nullptr || out_hit == nullptr || out_has_hit == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "physics_raycast: a required pointer is null");
    }
    CyQueryFilter whole{};
    if (const CyResult checked = check_ray(*ray); checked != CY_RESULT_OK) {
        return checked;
    }
    if (const CyResult normalised = normalise_filter(filter, whole); normalised != CY_RESULT_OK) {
        return normalised;
    }
    CyPhysicsHit hit{};
    bool has_hit = false;
    if (const CyResult answered = backend->raycast(*ray, whole, hit, has_hit);
        answered != CY_RESULT_OK) {
        return answered;
    }
    *out_hit = hit;
    *out_has_hit = has_hit;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult physics_raycast_all(CyEngine engine, const CyRay* ray, const CyQueryFilter* filter,
                             CyPhysicsHit* out_hits, uint32_t capacity, uint32_t* out_count) {
    PhysicsQueryBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, "physics_raycast_all", backend);
        backend == nullptr) {
        return entered;
    }
    if (ray == nullptr || out_count == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "physics_raycast_all: a required pointer is null");
    }
    CyQueryFilter whole{};
    if (const CyResult checked = check_ray(*ray); checked != CY_RESULT_OK) {
        return checked;
    }
    if (const CyResult normalised = normalise_filter(filter, whole); normalised != CY_RESULT_OK) {
        return normalised;
    }
    const Span<CyPhysicsHit> buffer =
        out_hits != nullptr ? Span<CyPhysicsHit>(out_hits, capacity) : Span<CyPhysicsHit>();
    u32 total = 0;
    if (const CyResult answered = backend->raycast_all(*ray, whole, buffer, total);
        answered != CY_RESULT_OK) {
        return answered;
    }
    return finish_sized(out_hits, capacity, total, out_count, "more hits than the buffer holds");
}

CyResult physics_shape_cast(CyEngine engine, const CyShape* shape, const CyPose* start,
                            const float* direction, float max_distance, const CyQueryFilter* filter,
                            CyPhysicsHit* out_hit, bool* out_has_hit) {
    PhysicsQueryBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, "physics_shape_cast", backend); backend == nullptr) {
        return entered;
    }
    if (shape == nullptr || start == nullptr || direction == nullptr || out_hit == nullptr ||
        out_has_hit == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "physics_shape_cast: a required pointer is null");
    }
    if (const CyResult checked = check_shape(*shape); checked != CY_RESULT_OK) {
        return checked;
    }
    if (const CyResult checked = check_pose(*start); checked != CY_RESULT_OK) {
        return checked;
    }
    if (!finite3(direction) ||
        (direction[0] == 0.0F && direction[1] == 0.0F && direction[2] == 0.0F)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "sweep direction must be finite and non-zero");
    }
    if (!finite_non_negative(max_distance)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "sweep max_distance must be finite and not negative");
    }
    CyQueryFilter whole{};
    if (const CyResult normalised = normalise_filter(filter, whole); normalised != CY_RESULT_OK) {
        return normalised;
    }
    CyPhysicsHit hit{};
    bool has_hit = false;
    if (const CyResult answered =
            backend->shape_cast(*shape, *start, direction, max_distance, whole, hit, has_hit);
        answered != CY_RESULT_OK) {
        return answered;
    }
    *out_hit = hit;
    *out_has_hit = has_hit;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult physics_overlap(CyEngine engine, const CyShape* shape, const CyPose* pose,
                         const CyQueryFilter* filter, CyEntity* out_entities, uint32_t capacity,
                         uint32_t* out_count) {
    PhysicsQueryBackend* backend = nullptr;
    if (const CyResult entered = enter(engine, "physics_overlap", backend); backend == nullptr) {
        return entered;
    }
    if (shape == nullptr || pose == nullptr || out_count == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "physics_overlap: a required pointer is null");
    }
    if (const CyResult checked = check_shape(*shape); checked != CY_RESULT_OK) {
        return checked;
    }
    if (const CyResult checked = check_pose(*pose); checked != CY_RESULT_OK) {
        return checked;
    }
    CyQueryFilter whole{};
    if (const CyResult normalised = normalise_filter(filter, whole); normalised != CY_RESULT_OK) {
        return normalised;
    }
    const Span<CyEntity> buffer =
        out_entities != nullptr ? Span<CyEntity>(out_entities, capacity) : Span<CyEntity>();
    u32 total = 0;
    if (const CyResult answered = backend->overlap(*shape, *pose, whole, buffer, total);
        answered != CY_RESULT_OK) {
        return answered;
    }
    return finish_sized(out_entities, capacity, total, out_count,
                        "more entities than the buffer holds");
}

}  // namespace cy::abi::game
