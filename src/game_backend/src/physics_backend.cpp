// SPDX-License-Identifier: MIT
// The `physics` adapter: `cy::abi::game::PhysicsQueryBackend` over `cy::physics::PhysicsServer`'s
// const queries. `add-swift-game-api`. See include/cy/game_backend/physics_backend.h for what it
// adds to the server's own answers, and tests/test_physics_backend.cpp for the proof.

#include <cy/game_backend/physics_backend.h>

#include <cy/abi/errors.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/server.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::game_backend {
namespace {

using physics::BodyHandle;
using physics::OverlapHit;
using physics::RayCastHit;

/// The first scratch size, and the most hits a list may hold before the adapter gives up growing.
constexpr usize kFirstScratch = 32;
constexpr usize kMaxScratch = usize{1} << 20U;
/// Ignore lists up to this long are resolved on the stack.
constexpr usize kInlineIgnore = 16;

Vec3 vec3(const float* value) noexcept {
    return Vec3{value[0], value[1], value[2]};
}

void write3(float* out, Vec3 value) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
}

/// `CyPose`'s rotation, with the all-zero quaternion read as the identity and anything else
/// normalised, so an author's slightly-off quaternion does not skew a sweep.
Quat rotation_of(const CyPose& pose) noexcept {
    const Quat raw{pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
    const f32 length_sq = (raw.x * raw.x) + (raw.y * raw.y) + (raw.z * raw.z) + (raw.w * raw.w);
    if (length_sq <= 1e-12F) {
        return Quat::identity();
    }
    return normalize(raw);
}

Transform transform_of(const CyPose& pose) noexcept {
    Transform transform = Transform::from_translation(vec3(pose.position));
    transform.rotation = rotation_of(pose);
    return transform;
}

physics::ShapeDescription description_of(const CyShape& shape) noexcept {
    physics::ShapeDescription description;
    switch (shape.kind) {
        case CY_SHAPE_CAPSULE:
            description.type = physics::ShapeType::Capsule;
            description.radius = shape.radius;
            description.half_height = shape.half_height;
            break;
        case CY_SHAPE_BOX:
            description.type = physics::ShapeType::Box;
            description.half_extents = vec3(shape.half_extents);
            break;
        default:
            description.type = physics::ShapeType::Sphere;
            description.radius = shape.radius;
            break;
    }
    // A query shape wants its exact surface, not one pushed out for the solver.
    description.convex_radius = 0.0F;
    return description;
}

/// Two cache keys are the same shape. Field by field: `CyShape` is floats, and bytes would call
/// 0.0 and -0.0 different shapes.
bool same_shape(const CyShape& a, const CyShape& b) noexcept {
    return a.kind == b.kind && a.radius == b.radius && a.half_height == b.half_height &&
           a.half_extents[0] == b.half_extents[0] && a.half_extents[1] == b.half_extents[1] &&
           a.half_extents[2] == b.half_extents[2];
}

/// `CyQueryFilter`'s flags and layers as the server's filter. `ignore` is filled by the caller.
physics::QueryFilter server_filter(const CyQueryFilter& filter) noexcept {
    physics::QueryFilter out;
    out.filter.layer = static_cast<u8>(filter.layer);
    out.filter.mask = filter.mask;
    out.include_triggers = (filter.flags & CY_QUERY_INCLUDE_TRIGGERS) != 0U;
    out.cull_back_faces = (filter.flags & CY_QUERY_HIT_BACK_FACES) == 0U;
    out.include_static = (filter.flags & CY_QUERY_SKIP_STATIC) == 0U;
    out.include_kinematic = (filter.flags & CY_QUERY_SKIP_KINEMATIC) == 0U;
    out.include_dynamic = (filter.flags & CY_QUERY_SKIP_DYNAMIC) == 0U;
    return out;
}

/// The ignore list's bodies, on the stack when short and on the heap otherwise. Entities without a
/// body resolve to nothing, which is right: there is nothing of theirs to hit.
class IgnoredBodies {
public:
    IgnoredBodies(Allocator& allocator, const EntityBodies& bodies,
                  const CyQueryFilter& filter) noexcept
        : heap_(allocator) {
        const usize count = filter.ignore_count;
        BodyHandle* target = inline_;
        if (count > kInlineIgnore) {
            if (!heap_.resize(count)) {
                failed_ = true;
                return;
            }
            target = heap_.data();
        }
        for (usize index = 0; index < count; ++index) {
            const BodyHandle body = bodies.body_of(filter.ignore[index]);
            if (!body.is_null()) {
                target[size_++] = body;
            }
        }
        data_ = target;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

    void apply(physics::QueryFilter& filter) const noexcept {
        filter.ignore = data_;
        filter.ignore_count = static_cast<u32>(size_);
    }

private:
    BodyHandle inline_[kInlineIgnore] = {};
    Array<BodyHandle> heap_;
    const BodyHandle* data_ = nullptr;
    usize size_ = 0;
    bool failed_ = false;
};

/// The total order every hit list is in: distance, entity, body handle.
bool ray_hit_before(const RayCastHit& a, const RayCastHit& b) noexcept {
    if (a.distance != b.distance) {
        return a.distance < b.distance;
    }
    if (a.user_data != b.user_data) {
        return a.user_data < b.user_data;
    }
    if (a.body.index() != b.body.index()) {
        return a.body.index() < b.body.index();
    }
    return a.body.generation() < b.body.generation();
}

CyPhysicsHit to_hit(const RayCastHit& hit, f32 max_distance) noexcept {
    CyPhysicsHit out{};
    out.flags = hit.trigger ? CY_HIT_TRIGGER : 0U;
    out.entity = static_cast<CyEntity>(hit.user_data);
    write3(out.point, hit.position);
    write3(out.normal, hit.normal);
    out.distance = hit.distance;
    out.fraction = max_distance > 0.0F ? hit.distance / max_distance : 0.0F;
    return out;
}

CyPhysicsHit to_hit(const physics::ShapeCastHit& hit) noexcept {
    CyPhysicsHit out{};
    out.flags = (hit.trigger ? CY_HIT_TRIGGER : 0U) |
                (hit.started_penetrating ? CY_HIT_STARTED_PENETRATING : 0U);
    out.entity = static_cast<CyEntity>(hit.user_data);
    write3(out.point, hit.position);
    write3(out.normal, hit.normal);
    out.distance = hit.distance;
    out.fraction = hit.fraction;
    return out;
}

/// Run `query(span)` into a growing scratch until the server stops filling it, so `scratch` ends
/// holding every result. `query` answers `Expected<u32, Error>`, the count written.
template <class T, class Query>
CyResult collect_all(Array<T>& scratch, Query&& query) noexcept {
    usize capacity = kFirstScratch;
    for (;;) {
        if (!scratch.resize(capacity)) {
            return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics query scratch");
        }
        const Expected<u32, Error> written = query(scratch.span());
        if (!written) {
            return abi::report(written.error());
        }
        if (*written < capacity || capacity >= kMaxScratch) {
            (void)scratch.resize(*written);  // shrinking never allocates
            return CY_RESULT_OK;
        }
        capacity *= 2U;
    }
}

}  // namespace

PhysicsQueryAdapter::PhysicsQueryAdapter(Allocator& allocator, physics::PhysicsServer& server,
                                         physics::WorldHandle world,
                                         const EntityBodies& bodies) noexcept
    : allocator_(&allocator),
      server_(&server),
      world_(world),
      bodies_(&bodies),
      shapes_(allocator) {}

PhysicsQueryAdapter::~PhysicsQueryAdapter() {
    for (const CachedShape& cached : shapes_.span()) {
        (void)server_->destroy_shape(cached.handle);
    }
}

u32 PhysicsQueryAdapter::cached_shapes() const noexcept {
    const std::lock_guard<std::mutex> lock(shapes_mutex_);
    return static_cast<u32>(shapes_.size());
}

CyResult PhysicsQueryAdapter::prewarm(const CyShape& shape) noexcept {
    physics::ShapeHandle handle;
    return shape_for(shape, handle);
}

CyResult PhysicsQueryAdapter::check_available() const noexcept {
    if (server_->stepping()) {
        return abi::report(CY_RESULT_UNAVAILABLE,
                           "the physics step is running; queries answer before or after it");
    }
    return CY_RESULT_OK;
}

CyResult PhysicsQueryAdapter::shape_for(const CyShape& shape,
                                        physics::ShapeHandle& out) const noexcept {
    CyShape key{};
    key.kind = shape.kind;
    // Only the fields the kind reads are part of the key, so a sphere with stale box extents in
    // its struct is still the same sphere.
    if (shape.kind == CY_SHAPE_BOX) {
        std::memcpy(key.half_extents, shape.half_extents, sizeof(key.half_extents));
    } else {
        key.radius = shape.radius;
        key.half_height = shape.kind == CY_SHAPE_CAPSULE ? shape.half_height : 0.0F;
    }

    const std::lock_guard<std::mutex> lock(shapes_mutex_);
    for (const CachedShape& cached : shapes_.span()) {
        if (same_shape(cached.key, key)) {
            out = cached.handle;
            return CY_RESULT_OK;
        }
    }
    const Expected<physics::ShapeHandle, Error> created =
        server_->create_shape(description_of(key));
    if (!created) {
        return abi::report(created.error());
    }
    if (!shapes_.push_back(CachedShape{key, *created})) {
        (void)server_->destroy_shape(*created);
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics query shape cache");
    }
    out = *created;
    return CY_RESULT_OK;
}

CyResult PhysicsQueryAdapter::raycast(const CyRay& ray, const CyQueryFilter& filter,
                                      CyPhysicsHit& out_hit, bool& out_has_hit) const noexcept {
    // The nearest hit is the first of the total order, so equal distances resolve by entity here
    // exactly as they do in `raycast_all`.
    CyPhysicsHit first{};
    u32 total = 0;
    if (const CyResult all = raycast_all(ray, filter, Span<CyPhysicsHit>(&first, 1), total);
        all != CY_RESULT_OK) {
        return all;
    }
    out_has_hit = total > 0U;
    out_hit = out_has_hit ? first : CyPhysicsHit{};
    return CY_RESULT_OK;
}

CyResult PhysicsQueryAdapter::raycast_all(const CyRay& ray, const CyQueryFilter& filter,
                                          Span<CyPhysicsHit> out, u32& out_total) const noexcept {
    if (const CyResult available = check_available(); available != CY_RESULT_OK) {
        return available;
    }
    const IgnoredBodies ignored(*allocator_, *bodies_, filter);
    if (ignored.failed()) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics query ignore list");
    }
    physics::QueryFilter query_filter = server_filter(filter);
    ignored.apply(query_filter);

    physics::RayCastInput input;
    input.origin = vec3(ray.origin);
    input.direction = vec3(ray.direction);
    input.max_distance = ray.max_distance;

    Array<RayCastHit> hits(*allocator_);
    if (const CyResult collected = collect_all(hits,
                                               [&](Span<RayCastHit> span) noexcept {
                                                   return server_->raycast_all(world_, input,
                                                                               query_filter, span);
                                               });
        collected != CY_RESULT_OK) {
        return collected;
    }
    std::ranges::sort(hits, ray_hit_before);
    const usize written = std::min(hits.size(), out.size());
    for (usize index = 0; index < written; ++index) {
        out[index] = to_hit(hits[index], ray.max_distance);
    }
    out_total = static_cast<u32>(hits.size());
    return CY_RESULT_OK;
}

CyResult PhysicsQueryAdapter::shape_cast(const CyShape& shape, const CyPose& start,
                                         const f32* direction, f32 max_distance,
                                         const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                                         bool& out_has_hit) const noexcept {
    if (const CyResult available = check_available(); available != CY_RESULT_OK) {
        return available;
    }
    physics::ShapeCastInput input;
    if (const CyResult resolved = shape_for(shape, input.shape); resolved != CY_RESULT_OK) {
        return resolved;
    }
    const IgnoredBodies ignored(*allocator_, *bodies_, filter);
    if (ignored.failed()) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics query ignore list");
    }
    physics::QueryFilter query_filter = server_filter(filter);
    ignored.apply(query_filter);

    input.start = transform_of(start);
    input.direction = vec3(direction);
    input.max_distance = max_distance;
    const Expected<physics::ShapeCastHit, Error> hit =
        server_->shape_cast(world_, input, query_filter);
    if (!hit) {
        return abi::report(hit.error());
    }
    out_has_hit = !hit->body.is_null();
    out_hit = out_has_hit ? to_hit(*hit) : CyPhysicsHit{};
    return CY_RESULT_OK;
}

CyResult PhysicsQueryAdapter::overlap(const CyShape& shape, const CyPose& pose,
                                      const CyQueryFilter& filter, Span<CyEntity> out,
                                      u32& out_total) const noexcept {
    if (const CyResult available = check_available(); available != CY_RESULT_OK) {
        return available;
    }
    physics::OverlapInput input;
    if (const CyResult resolved = shape_for(shape, input.shape); resolved != CY_RESULT_OK) {
        return resolved;
    }
    const IgnoredBodies ignored(*allocator_, *bodies_, filter);
    if (ignored.failed()) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics query ignore list");
    }
    physics::QueryFilter query_filter = server_filter(filter);
    ignored.apply(query_filter);
    input.transform = transform_of(pose);

    Array<OverlapHit> hits(*allocator_);
    if (const CyResult collected = collect_all(hits,
                                               [&](Span<OverlapHit> span) noexcept {
                                                   return server_->overlap(world_, input,
                                                                           query_filter, span);
                                               });
        collected != CY_RESULT_OK) {
        return collected;
    }

    // Entity order, each entity once, bodies without an entity dropped.
    Array<CyEntity> entities(*allocator_);
    if (!entities.reserve(hits.size())) {
        return abi::report(CY_RESULT_OUT_OF_MEMORY, "physics overlap entities");
    }
    for (const OverlapHit& hit : hits.span()) {
        if (hit.user_data != 0U) {
            (void)entities.push_back(static_cast<CyEntity>(hit.user_data));
        }
    }
    std::ranges::sort(entities);
    const auto duplicates = std::ranges::unique(entities);
    const auto total = static_cast<usize>(duplicates.begin() - entities.begin());
    const usize written = std::min(total, out.size());
    for (usize index = 0; index < written; ++index) {
        out[index] = entities[index];
    }
    out_total = static_cast<u32>(total);
    return CY_RESULT_OK;
}

void bind(cy::abi::Host& host, PhysicsQueryAdapter* adapter) noexcept {
    host.game.physics = adapter;
}

}  // namespace cy::game_backend
