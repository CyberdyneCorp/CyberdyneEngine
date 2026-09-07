#include <cy/rendering/raytracing/acceleration.h>

#include <cy/core/jobs/types.h>
#include <cy/core/math/geometry.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace cy::rendering::rt {
namespace {

/// The bytes a built structure is accounted at.
///
/// A device's structure is opaque and its size is a driver decision, so any number here is a model.
/// This one is the model the diagnostics are honest about: the vertex and index data the service
/// copied, plus the tree it built over them. It moves with the geometry the way a real structure
/// does, which is what makes "attribute structure memory to geometry sources and assets" answerable
/// — and it is not a device allocation, which is why the field is documented as accounted rather
/// than measured.
[[nodiscard]] u64 structure_bytes_of(usize positions, usize indices, usize nodes) noexcept {
    return (static_cast<u64>(positions) * sizeof(Vec3)) +
           (static_cast<u64>(indices) * sizeof(u32)) + (static_cast<u64>(nodes) * 32U);
}

[[nodiscard]] Aabb bounds_of(Span<const Vec3> positions) noexcept {
    if (positions.empty()) {
        return Aabb{};
    }
    Aabb box = Aabb::from_point(positions[0]);
    for (usize index = 1; index < positions.size(); ++index) {
        box = merge(box, Aabb::from_point(positions[index]));
    }
    return box;
}

/// The largest extent of `a` divided by the largest extent of `b`, or 1 when `b` is degenerate.
///
/// The refit quality measure: a structure refit from a pose that has grown far outside the bounds
/// it was built for has cells full of triangles that are nowhere near them, and the adapter's
/// `refit_quality_floor` is the ratio at which rebuilding is cheaper than tracing that.
[[nodiscard]] f32 bounds_growth(const Aabb& fresh, const Aabb& built) noexcept {
    const Vec3 built_size = built.size();
    const f32 built_extent = std::max({built_size.x, built_size.y, built_size.z});
    if (built_extent <= 0.0F) {
        return 1.0F;
    }
    const Vec3 fresh_size = fresh.size();
    const f32 fresh_extent = std::max({fresh_size.x, fresh_size.y, fresh_size.z});
    return fresh_extent / built_extent;
}

}  // namespace

const char* availability_name(Availability availability) noexcept {
    switch (availability) {
        case Availability::Unsupported:
            return "Unsupported";
        case Availability::DisabledByProfile:
            return "DisabledByProfile";
        case Availability::Available:
            return "Available";
    }
    return "Unknown";
}

const char* consumer_name(Consumer consumer) noexcept {
    switch (consumer) {
        case Consumer::GlobalIllumination:
            return "GlobalIllumination";
        case Consumer::Reflections:
            return "Reflections";
        case Consumer::Shadows:
            return "Shadows";
        case Consumer::AmbientOcclusion:
            return "AmbientOcclusion";
        case Consumer::Gameplay:
            return "Gameplay";
        case Consumer::Count:
            break;
    }
    return "Unknown";
}

AccelerationService::AccelerationService(const ServiceConfig& config) noexcept
    : budget_(config.budget) {
    if (!config.device_supports_ray_tracing) {
        availability_ = Availability::Unsupported;
    } else if (!config.enabled_by_profile) {
        availability_ = Availability::DisabledByProfile;
    } else {
        availability_ = Availability::Available;
    }
}

void AccelerationService::set_enabled_by_profile(bool enabled) noexcept {
    if (availability_ == Availability::Unsupported) {
        // A device without the extension does not acquire it because a profile asked. The three
        // states are a cause, and this one is not the profile's to change.
        return;
    }
    availability_ = enabled ? Availability::Available : Availability::DisabledByProfile;
}

u32 AccelerationService::find_bottom(GeometryId id) const noexcept {
    for (usize index = 0; index < bottoms_.size(); ++index) {
        if (bottoms_[index].live && bottoms_[index].id == id) {
            return static_cast<u32>(index);
        }
    }
    return kInvalidHandle;
}

Status AccelerationService::store_geometry(Bottom& bottom, const BuildInput& input) noexcept {
    bottom.input = input;
    // The spans in `input` belong to the caller and are valid for this call only; everything the
    // service will need later is copied here and the spans are cleared so nothing can read them
    // back. This is the one place a dangling span could hide, so it does not survive the function.
    bottom.input.triangles = TriangleGeometry{};

    bottom.positions.clear();
    bottom.indices.clear();
    bottom.triangle_materials.clear();
    if (Status appended = bottom.positions.append(input.triangles.positions); !appended) {
        return appended;
    }
    if (Status appended = bottom.indices.append(input.triangles.indices); !appended) {
        return appended;
    }
    if (Status appended = bottom.triangle_materials.append(input.triangles.triangle_materials);
        !appended) {
        return appended;
    }
    bottom.bounds = input.procedural ? input.procedural_bounds : bounds_of(bottom.positions.span());
    return ok();
}

Status AccelerationService::declare_geometry(GeometryId id, const BuildInput& input) noexcept {
    if (!input.procedural && (input.triangles.indices.size() % 3) != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "declare_geometry: an index count that is not a multiple of three is not a "
                    "triangle list");
    }
    if (input.maintenance == MaintenancePolicy::InstanceOnly) {
        return fail(ErrorCode::InvalidArgument,
                    "declare_geometry: an InstanceOnly source owns no structure of its own; add "
                    "instances referencing the source mesh's geometry instead");
    }

    u32 slot = find_bottom(id);
    if (slot == kInvalidHandle) {
        for (usize index = 0; index < bottoms_.size(); ++index) {
            if (!bottoms_[index].live) {
                slot = static_cast<u32>(index);
                break;
            }
        }
    }
    if (slot == kInvalidHandle) {
        Bottom fresh;
        if (Status pushed = bottoms_.push_back(std::move(fresh)); !pushed) {
            return pushed;
        }
        slot = static_cast<u32>(bottoms_.size() - 1);
    }

    Bottom& bottom = bottoms_[slot];
    const bool reused = bottom.live && bottom.id == id;
    bottom.id = id;
    bottom.live = true;
    bottom.declared = true;
    if (!reused) {
        bottom.instance_refs = 0;
    }
    if (Status stored = store_geometry(bottom, input); !stored) {
        return stored;
    }
    bottom.built = false;
    bottom.needs_build = true;
    bottom.needs_refit = false;
    return ok();
}

void AccelerationService::release_geometry(GeometryId id) noexcept {
    const u32 slot = find_bottom(id);
    if (slot != kInvalidHandle) {
        bottoms_[slot].declared = false;
    }
}

bool AccelerationService::has_structure(GeometryId id) const noexcept {
    const u32 slot = find_bottom(id);
    return slot != kInvalidHandle && bottoms_[slot].built;
}

Status AccelerationService::refit_geometry(GeometryId id, Span<const Vec3> positions) noexcept {
    const u32 slot = find_bottom(id);
    if (slot == kInvalidHandle) {
        return fail(ErrorCode::NotFound, "refit_geometry: no such geometry");
    }
    Bottom& bottom = bottoms_[slot];
    if (bottom.input.maintenance != MaintenancePolicy::Refit) {
        return fail(ErrorCode::InvalidArgument,
                    "refit_geometry: this geometry's adapter does not declare a refit policy");
    }
    if (positions.size() != bottom.positions.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "refit_geometry: a refit keeps the topology, so the vertex count may not "
                    "change; declare the geometry again for a rebuild");
    }
    for (usize index = 0; index < positions.size(); ++index) {
        bottom.positions[index] = positions[index];
    }
    bottom.bounds = bounds_of(bottom.positions.span());
    bottom.needs_refit = true;
    return ok();
}

Expected<u32, Error> AccelerationService::add_instance(
    const InstanceDescriptor& descriptor) noexcept {
    const u32 bottom = find_bottom(descriptor.geometry);
    if (bottom == kInvalidHandle) {
        return fail(ErrorCode::NotFound,
                    "add_instance: the geometry has not been declared; an instance may not outlive "
                    "the declaration its structure is shared through");
    }
    Expected<Mat4, Error> inverse = inverse_affine(descriptor.transform);
    if (!inverse) {
        return make_unexpected(inverse.error());
    }

    u32 handle = kInvalidHandle;
    if (!free_instances_.empty()) {
        handle = free_instances_.back();
        free_instances_.pop_back();
    } else {
        Instance fresh;
        if (Status pushed = instances_.push_back(fresh); !pushed) {
            return make_unexpected(pushed.error());
        }
        handle = static_cast<u32>(instances_.size() - 1);
    }

    Instance& instance = instances_[handle];
    instance.descriptor = descriptor;
    instance.inverse_transform = inverse.value();
    instance.bottom = bottom;
    instance.live = true;
    bottoms_[bottom].instance_refs += 1;
    top_dirty_ = true;
    return handle;
}

void AccelerationService::remove_instance(u32 handle) noexcept {
    if (handle >= instances_.size() || !instances_[handle].live) {
        return;
    }
    Instance& instance = instances_[handle];
    if (instance.bottom != kInvalidHandle && bottoms_[instance.bottom].instance_refs > 0) {
        bottoms_[instance.bottom].instance_refs -= 1;
    }
    instance.live = false;
    instance.bottom = kInvalidHandle;
    (void)free_instances_.push_back(handle);
    top_dirty_ = true;
}

Status AccelerationService::set_instance_transform(u32 handle, const Mat4& transform) noexcept {
    if (handle >= instances_.size() || !instances_[handle].live) {
        return fail(ErrorCode::NotFound, "set_instance_transform: no such instance");
    }
    Expected<Mat4, Error> inverse = inverse_affine(transform);
    if (!inverse) {
        return make_unexpected(inverse.error());
    }
    instances_[handle].descriptor.transform = transform;
    instances_[handle].inverse_transform = inverse.value();
    top_dirty_ = true;
    return ok();
}

Status AccelerationService::build_bottom(Bottom& bottom) noexcept {
    bottom.tree.clear();
    if (bottom.input.procedural) {
        // Bounds plus an intersection shader: there is no triangle tree to build, and the top level
        // holds the bounds. The service records the structure so the instance is traceable, and
        // `trace_instance` reports the procedural bounds hit rather than inventing a surface.
        bottom.built = true;
        bottom.needs_build = false;
        bottom.built_bounds = bottom.bounds;
        bottom.bytes = structure_bytes_of(0, 0, 1);
        return ok();
    }

    const usize triangles = bottom.indices.size() / 3;
    if (triangles == 0) {
        bottom.built = true;
        bottom.needs_build = false;
        bottom.built_bounds = bottom.bounds;
        bottom.bytes = 0;
        return ok();
    }

    Array<Aabb> bounds;
    Array<u32> payloads;
    if (Status reserved = bounds.reserve(triangles); !reserved) {
        return reserved;
    }
    if (Status reserved = payloads.reserve(triangles); !reserved) {
        return reserved;
    }
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        const Vec3 v0 = bottom.positions[bottom.indices[(triangle * 3) + 0]];
        const Vec3 v1 = bottom.positions[bottom.indices[(triangle * 3) + 1]];
        const Vec3 v2 = bottom.positions[bottom.indices[(triangle * 3) + 2]];
        Aabb box = Aabb::from_point(v0);
        box = merge(box, Aabb::from_point(v1));
        box = merge(box, Aabb::from_point(v2));
        if (Status pushed = bounds.push_back(box); !pushed) {
            return pushed;
        }
        if (Status pushed = payloads.push_back(static_cast<u32>(triangle)); !pushed) {
            return pushed;
        }
    }
    if (Status built = bottom.tree.build(bounds.data(), payloads.data(), triangles); !built) {
        return built;
    }

    bottom.built = true;
    bottom.needs_build = false;
    bottom.needs_refit = false;
    bottom.built_bounds = bottom.bounds;
    bottom.bytes = structure_bytes_of(bottom.positions.size(), bottom.indices.size(),
                                      bottom.tree.node_count());
    return ok();
}

void AccelerationService::evict_unreferenced() noexcept {
    for (Bottom& bottom : bottoms_) {
        if (!bottom.live || bottom.declared || bottom.instance_refs != 0) {
            continue;
        }
        bottom.tree.clear();
        bottom.positions.clear();
        bottom.indices.clear();
        bottom.triangle_materials.clear();
        bottom.built = false;
        bottom.needs_build = false;
        bottom.needs_refit = false;
        bottom.live = false;
        bottom.bytes = 0;
        top_dirty_ = true;
    }
}

void AccelerationService::service_refits(FrameReport& report) noexcept {
    u32 refits = 0;
    for (Bottom& bottom : bottoms_) {
        if (!bottom.live || !bottom.needs_refit) {
            continue;
        }
        if (refits >= budget_.max_refits_per_frame) {
            continue;
        }
        const Adapter adapter = adapter_for(bottom.input.source);
        const f32 growth = bounds_growth(bottom.bounds, bottom.built_bounds);
        // "with a full rebuild only when refit quality degrades beyond a threshold". Growth is
        // measured against the bounds the tree was BUILT for, not against last frame's, so a pose
        // that drifts a little every frame still triggers one rebuild rather than never.
        if (adapter.refit_quality_floor > 0.0F && growth > 1.0F + adapter.refit_quality_floor) {
            bottom.needs_build = true;
            bottom.needs_refit = false;
            continue;
        }
        // A refit keeps the tree's topology and moves its bounds. The engine's `Bvh` is built, not
        // refit, so the refit is expressed as a rebuild of the same tree at refit cost — which is
        // what a device driver does underneath `vkCmdBuildAccelerationStructuresKHR` with the
        // update flag, and what keeps the traced result exact rather than approximate.
        if (Status built = build_bottom(bottom); !built) {
            continue;
        }
        bottom.needs_refit = false;
        refits += 1;
        report.refits += 1;
        top_dirty_ = true;
    }
}

void AccelerationService::service_builds(FrameReport& report) noexcept {
    // Priority: the best importance and coverage among the instances waiting on this geometry.
    for (Bottom& bottom : bottoms_) {
        bottom.priority = 0.0F;
    }
    for (const Instance& instance : instances_) {
        if (!instance.live || instance.bottom == kInvalidHandle) {
            continue;
        }
        Bottom& bottom = bottoms_[instance.bottom];
        const f32 priority =
            instance.descriptor.importance + (instance.descriptor.screen_coverage * 2.0F);
        bottom.priority = std::max(bottom.priority, priority);
    }

    Array<u32> queue;
    for (usize index = 0; index < bottoms_.size(); ++index) {
        if (bottoms_[index].live && bottoms_[index].needs_build) {
            (void)queue.push_back(static_cast<u32>(index));
        }
    }
    std::ranges::sort(queue, [this](u32 a, u32 b) {
        if (bottoms_[a].priority != bottoms_[b].priority) {
            return bottoms_[a].priority > bottoms_[b].priority;
        }
        // A stable tie-break by identity, so a frame's build order does not depend on the order
        // declarations happened to arrive in.
        return bottoms_[a].id < bottoms_[b].id;
    });

    u32 builds = 0;
    u64 triangles = 0;
    for (const u32 slot : queue) {
        Bottom& bottom = bottoms_[slot];
        const u64 cost = static_cast<u64>(bottom.indices.size() / 3);
        const bool over_count = builds >= budget_.max_builds_per_frame;
        const bool over_cost =
            builds > 0 && (triangles + cost) > budget_.max_build_triangles_per_frame;
        if (over_count || over_cost) {
            report.deferred_builds += 1;
            continue;
        }
        const bool rebuild = bottom.bytes != 0;
        if (Status built = build_bottom(bottom); !built) {
            continue;
        }
        builds += 1;
        triangles += cost;
        if (rebuild) {
            report.rebuilds += 1;
        } else {
            report.builds += 1;
        }
        top_dirty_ = true;
    }
}

void AccelerationService::rebuild_top_level(FrameReport& report) noexcept {
    const i64 started = jobs::monotonic_now_ns();
    top_bounds_.clear();
    top_payloads_.clear();
    for (usize index = 0; index < instances_.size(); ++index) {
        const Instance& instance = instances_[index];
        if (!instance.live || instance.bottom == kInvalidHandle) {
            continue;
        }
        const Bottom& bottom = bottoms_[instance.bottom];
        if (!bottom.built) {
            // "instances awaiting a structure SHALL be excluded from tracing rather than stalling
            // the frame".
            report.excluded_instances += 1;
            continue;
        }
        (void)top_bounds_.push_back(transformed(bottom.bounds, instance.descriptor.transform));
        (void)top_payloads_.push_back(static_cast<u32>(index));
    }

    top_.clear();
    if (!top_payloads_.empty()) {
        (void)top_.build(top_bounds_.data(), top_payloads_.data(), top_payloads_.size(), 2);
    }
    report.traced_instances = static_cast<u32>(top_payloads_.size());
    top_dirty_ = false;
    report.top_level_rebuild_ns = static_cast<u64>(jobs::monotonic_now_ns() - started);
}

void AccelerationService::account_structure_bytes() noexcept {
    diagnostics_.structure_bytes = 0;
    for (u64& bytes : diagnostics_.structure_bytes_by_source) {
        bytes = 0;
    }
    u32 count = 0;
    for (const Bottom& bottom : bottoms_) {
        if (!bottom.live || !bottom.built) {
            continue;
        }
        count += 1;
        diagnostics_.structure_bytes += bottom.bytes;
        diagnostics_.structure_bytes_by_source[static_cast<u32>(bottom.input.source)] +=
            bottom.bytes;
    }
    diagnostics_.bottom_level_count = count;
}

FrameReport AccelerationService::update() noexcept {
    FrameReport report;
    if (!active()) {
        // Inactive is inactive: nothing is built, nothing is traced, and the diagnostics say zero.
        // This is the branch that makes `DisabledByProfile` indistinguishable from `Unsupported`.
        diagnostics_.last_frame = report;
        diagnostics_.structure_bytes = 0;
        diagnostics_.bottom_level_count = 0;
        diagnostics_.instance_count = 0;
        diagnostics_.pending_builds = 0;
        return report;
    }

    evict_unreferenced();
    service_refits(report);
    service_builds(report);
    rebuild_top_level(report);
    account_structure_bytes();

    u32 live_instances = 0;
    for (const Instance& instance : instances_) {
        if (instance.live) {
            live_instances += 1;
        }
    }
    u32 pending = 0;
    for (const Bottom& bottom : bottoms_) {
        if (bottom.live && (bottom.needs_build || bottom.needs_refit)) {
            pending += 1;
        }
    }
    diagnostics_.instance_count = live_instances;
    diagnostics_.pending_builds = pending;
    diagnostics_.last_frame = report;
    return report;
}

RayHit AccelerationService::trace_instance(const Instance& instance, const RayQuery& query,
                                           f32 t_max) const noexcept {
    RayHit hit;
    const Bottom& bottom = bottoms_[instance.bottom];

    // Into the geometry's own space. The direction is renormalised and the parameter scaled back,
    // so `t` is a world-space distance whatever scale the instance carries — a caller comparing two
    // hits from two instances is comparing metres.
    Ray local;
    local.origin = transform_point(instance.inverse_transform, query.ray.origin);
    const Vec3 scaled = transform_direction(instance.inverse_transform, query.ray.direction);
    const f32 scale = length(scaled);
    if (scale <= 0.0F) {
        return hit;
    }
    local.direction = scaled / scale;
    const f32 local_t_max = t_max * scale;

    if (bottom.input.procedural) {
        // Bounds plus an intersection shader. This layer has no shader, so it reports the bounds
        // hit and the shader id travels on the geometry: a consumer that needs the exact surface
        // dispatches the intersection shader itself.
        f32 enter = 0.0F;
        f32 exit = 0.0F;
        if (!geom::ray_aabb(local, bottom.bounds, local_t_max, enter, exit)) {
            return hit;
        }
        hit.hit = true;
        hit.t = enter / scale;
        hit.position = query.ray.at(hit.t);
        hit.normal = -query.ray.direction;
        hit.instance_id = instance.descriptor.instance_id;
        hit.material_id = instance.descriptor.material_id;
        hit.declared_error_metres = bottom.input.declared_error_metres;
        return hit;
    }

    f32 nearest = local_t_max;
    u32 nearest_triangle = 0;
    geom::TriangleHit nearest_hit;
    bool found = false;
    const bool any = query.kind != QueryKind::ClosestHit;

    bottom.tree.query_ray(local, local_t_max, [&](const u32& triangle, const Aabb&) {
        const Vec3 v0 = bottom.positions[bottom.indices[(triangle * 3) + 0]];
        const Vec3 v1 = bottom.positions[bottom.indices[(triangle * 3) + 1]];
        const Vec3 v2 = bottom.positions[bottom.indices[(triangle * 3) + 2]];
        geom::TriangleHit candidate;
        if (!geom::ray_triangle(local, v0, v1, v2, nearest, query.cull_back_faces, candidate)) {
            return true;
        }
        found = true;
        nearest = candidate.t;
        nearest_triangle = triangle;
        nearest_hit = candidate;
        // An any-hit or shadow query stops at the first surface it meets.
        return !any;
    });

    if (!found) {
        return hit;
    }

    hit.hit = true;
    hit.t = nearest / scale;
    hit.primitive_index = nearest_triangle;
    hit.bary_u = nearest_hit.u;
    hit.bary_v = nearest_hit.v;
    hit.instance_id = instance.descriptor.instance_id;
    hit.material_id = bottom.triangle_materials.empty()
                          ? instance.descriptor.material_id
                          : bottom.triangle_materials[nearest_triangle];
    hit.declared_error_metres = bottom.input.declared_error_metres;
    hit.position = query.ray.at(hit.t);

    const Vec3 v0 = bottom.positions[bottom.indices[(nearest_triangle * 3) + 0]];
    const Vec3 v1 = bottom.positions[bottom.indices[(nearest_triangle * 3) + 1]];
    const Vec3 v2 = bottom.positions[bottom.indices[(nearest_triangle * 3) + 2]];
    const Mat3 normal_transform = normal_matrix(instance.descriptor.transform.upper3x3());
    Vec3 normal = normalized_or(normal_transform * cross(v1 - v0, v2 - v0), Vec3{0.0F, 1.0F, 0.0F});
    if (dot(normal, query.ray.direction) > 0.0F) {
        normal = -normal;
    }
    hit.normal = normal;
    return hit;
}

RayHit AccelerationService::trace(const RayQuery& query) noexcept {
    if (!active()) {
        diagnostics_.queries_while_inactive += 1;
        return RayHit{};
    }
    diagnostics_.rays[static_cast<u32>(query.consumer)] += 1;
    if (top_payloads_.empty()) {
        return RayHit{};
    }

    Ray offset_ray = query.ray;
    offset_ray.origin = query.ray.at(query.t_min);
    const f32 span = query.t_max - query.t_min;
    if (span <= 0.0F) {
        return RayHit{};
    }

    RayHit best;
    f32 nearest = span;
    const bool any = query.kind != QueryKind::ClosestHit;
    RayQuery local_query = query;
    local_query.ray = offset_ray;

    top_.query_ray(offset_ray, span, [&](const u32& slot, const Aabb&) {
        const Instance& instance = instances_[slot];
        const RayHit candidate = trace_instance(instance, local_query, nearest);
        if (candidate.hit && candidate.t < nearest) {
            nearest = candidate.t;
            best = candidate;
        }
        return !(any && best.hit);
    });

    if (best.hit) {
        best.t += query.t_min;
        best.position = query.ray.at(best.t);
    }
    return best;
}

bool AccelerationService::occluded(const Ray& ray, f32 t_max, Consumer consumer) noexcept {
    RayQuery query;
    query.ray = ray;
    query.t_max = t_max;
    query.kind = QueryKind::Shadow;
    query.consumer = consumer;
    return trace(query).hit;
}

void AccelerationService::reset_ray_counts() noexcept {
    for (u64& count : diagnostics_.rays) {
        count = 0;
    }
    diagnostics_.queries_while_inactive = 0;
}

}  // namespace cy::rendering::rt
