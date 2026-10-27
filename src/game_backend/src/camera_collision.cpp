// SPDX-License-Identifier: MIT
// The camera's collision and occlusion queries, answered by a physics world. See
// cy/game_backend/camera_collision.h.

#include <cy/game_backend/camera_collision.h>

#include <cy/core/math/scalar.h>

namespace cy::game_backend {
namespace {

/// A segment shorter than this has no direction to sweep along: its answer is "clear".
constexpr f32 kShortestSegment = 1e-4F;

}  // namespace

CameraCollisionResolver::CameraCollisionResolver(Allocator& allocator,
                                                 physics::PhysicsServer& server,
                                                 physics::WorldHandle world) noexcept
    : server_(&server),
      world_(world),
      ignored_(allocator),
      probes_(allocator),
      results_(allocator) {}

CameraCollisionResolver::~CameraCollisionResolver() {
    for (const Probe& probe : probes_) {
        (void)server_->destroy_shape(probe.shape);
    }
}

Status CameraCollisionResolver::set_ignored(Span<const physics::BodyHandle> bodies) noexcept {
    ignored_.clear();
    return ignored_.append(bodies);
}

physics::QueryFilter CameraCollisionResolver::query_filter() const noexcept {
    physics::QueryFilter filter;
    filter.filter = filter_;
    filter.ignore = ignored_.data();
    filter.ignore_count = static_cast<u32>(ignored_.size());
    return filter;
}

Expected<physics::ShapeHandle, Error> CameraCollisionResolver::probe(f32 radius) noexcept {
    for (const Probe& probe : probes_) {
        if (probe.radius == radius) {
            return probe.shape;
        }
    }
    physics::ShapeDescription sphere;
    sphere.type = physics::ShapeType::Sphere;
    sphere.radius = radius;
    Expected<physics::ShapeHandle, Error> made = server_->create_shape(sphere);
    if (!made) {
        return made;
    }
    if (Status kept = probes_.push_back(Probe{radius, *made}); !kept) {
        (void)server_->destroy_shape(*made);
        return make_unexpected(kept.error());
    }
    return made;
}

Expected<camera::CameraQueryResult, Error> CameraCollisionResolver::collide(
    const camera::CameraQuery& query) noexcept {
    camera::CameraQueryResult result;
    result.kind = query.kind;
    result.rig_bits = query.rig_bits;
    const Vec3 segment = query.to - query.from;
    const f32 length = cy::length(segment);
    if (length < kShortestSegment) {
        return result;
    }
    const Vec3 direction = segment * (1.0F / length);
    physics::ShapeCastHit hit;
    if (query.radius > 0.0F) {
        Expected<physics::ShapeHandle, Error> shape = probe(query.radius);
        if (!shape) {
            return make_unexpected(shape.error());
        }
        physics::ShapeCastInput sweep;
        sweep.shape = *shape;
        sweep.start = Transform::from_translation(query.from);
        sweep.direction = direction;
        sweep.max_distance = length;
        Expected<physics::ShapeCastHit, Error> swept =
            server_->shape_cast(world_, sweep, query_filter());
        if (!swept) {
            return make_unexpected(swept.error());
        }
        hit = *swept;
    } else {
        physics::RayCastInput ray;
        ray.origin = query.from;
        ray.direction = direction;
        ray.max_distance = length;
        Expected<physics::RayCastHit, Error> cast = server_->raycast(world_, ray, query_filter());
        if (!cast) {
            return make_unexpected(cast.error());
        }
        hit.body = cast->body;
        hit.normal = cast->normal;
        hit.distance = cast->distance;
    }
    if (hit.body.is_null()) {
        return result;
    }
    // A probe that starts inside something has nowhere to go: the camera comes all the way in.
    result.fraction =
        hit.started_penetrating ? 0.0F : math::clamp(hit.distance / length, 0.0F, 1.0F);
    result.normal = hit.normal;
    return result;
}

Expected<camera::CameraQueryResult, Error> CameraCollisionResolver::occlude(
    const camera::CameraQuery& query) const noexcept {
    camera::CameraQueryResult result;
    result.kind = query.kind;
    result.rig_bits = query.rig_bits;
    const Vec3 segment = query.to - query.from;
    const f32 length = cy::length(segment);
    if (length < kShortestSegment) {
        return result;
    }
    physics::RayCastInput ray;
    ray.origin = query.from;
    ray.direction = segment * (1.0F / length);
    ray.max_distance = length;
    Expected<physics::RayCastHit, Error> cast = server_->raycast(world_, ray, query_filter());
    if (!cast) {
        return make_unexpected(cast.error());
    }
    if (!cast->body.is_null()) {
        result.blocked_fraction = 1.0F;
        result.fraction = math::clamp(cast->distance / length, 0.0F, 1.0F);
        result.normal = cast->normal;
    }
    return result;
}

Status CameraCollisionResolver::resolve(camera::CameraServer& camera) noexcept {
    stats_ = Stats{};
    results_.clear();
    for (const camera::CameraQuery& query : camera.queries()) {
        const bool collision = query.kind == camera::CameraQuery::Kind::Collision;
        Expected<camera::CameraQueryResult, Error> answered =
            collision ? collide(query) : occlude(query);
        if (!answered) {
            return Status{make_unexpected(answered.error())};
        }
        if (collision) {
            ++stats_.collision_queries;
            stats_.collisions += answered->fraction < 1.0F ? 1U : 0U;
            stats_.nearest_fraction = math::min(stats_.nearest_fraction, answered->fraction);
        } else {
            ++stats_.occlusion_queries;
            stats_.occlusions += answered->blocked_fraction > 0.0F ? 1U : 0U;
        }
        if (Status kept = results_.push_back(*answered); !kept) {
            return kept;
        }
    }
    return camera.submit_query_results(results_.span());
}

}  // namespace cy::game_backend
