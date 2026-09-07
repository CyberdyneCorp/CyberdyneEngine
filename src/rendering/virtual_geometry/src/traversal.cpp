#include <cy/rendering/virtual_geometry/traversal.h>

#include <cy/core/memory/hash_map.h>

#include <cmath>

namespace cy::rendering::vg {

namespace {

/// The camera, in one instance's local space.
///
/// EVERY PROJECTION IS DONE IN LOCAL SPACE, and the reason is worth stating because it looks like a
/// shortcut and is the opposite. A cluster's error is a world-space distance in the ASSET's units;
/// an instance scales both the error and the distance to the camera by the same factor, and the
/// screen-error projection divides one by the other. The scale cancels exactly. So transforming the
/// camera into local space once per instance gives the same number as transforming every cluster's
/// sphere into world space, and it is one transform per instance instead of two per cluster.
[[nodiscard]] Vec3 camera_in_local_space(const GeometryInstance& instance, Vec3 camera) noexcept {
    const f32 inverse_scale = instance.scale > 1.0e-8F ? 1.0F / instance.scale : 1.0F;
    const Quat conjugate{-instance.rotation.x, -instance.rotation.y, -instance.rotation.z,
                         instance.rotation.w};
    return (conjugate * (camera - instance.translation)) * inverse_scale;
}

[[nodiscard]] Sphere to_world(const GeometryInstance& instance, Vec3 center, f32 radius) noexcept {
    Sphere sphere;
    sphere.center = ((instance.rotation * center) * instance.scale) + instance.translation;
    sphere.radius = radius * instance.scale;
    return sphere;
}

[[nodiscard]] Sphere bounds_sphere(const Aabb& box) noexcept {
    Sphere sphere;
    sphere.center = box.center();
    sphere.radius = length(box.size()) * 0.5F;
    return sphere;
}

/// The projected diameter of a world-space sphere, in pixels.
[[nodiscard]] f32 screen_diameter(const ProjectionView& view, const Sphere& sphere) noexcept {
    ErrorSphere as_error;
    as_error.center = sphere.center;
    as_error.radius = sphere.radius;
    return 2.0F * sphere.radius * view.pixels_per_unit_at(as_error);
}

/// The per-instance working state of one traversal. A struct so that the descent below reads as the
/// descent rather than as nine parameters.
struct InstanceWalk {
    const DecodedAsset& asset;
    const GeometryInstance& instance;
    const TraversalView& view;
    const TraversalInputs& inputs;
    ProjectionView local;
    f32 threshold = 1.0F;
    u32 instance_index = 0;
};

[[nodiscard]] bool page_is_resident(const InstanceWalk& walk, u32 page) noexcept {
    return walk.inputs.resident(walk.instance.asset, page, walk.inputs.resident_user);
}

}  // namespace

bool all_pages_resident(u32 /*asset*/, u32 /*page*/, void* /*user*/) noexcept {
    return true;
}

TraversalResult::TraversalResult(Allocator& allocator) noexcept
    : visible(allocator), requests(allocator) {}

void TraversalResult::clear() noexcept {
    visible.clear();
    requests.clear();
    stats = TraversalStatistics{};
}

namespace {

/// Record that a page was wanted and could not be had. Requests are accumulated per page rather
/// than per cluster: "Requests SHALL be compacted and deduplicated on the GPU before the CPU reads
/// them", and the reference does the same so that the two lists compare.
[[nodiscard]] Status note_request(TraversalResult& out, HashMap<u64, u32>& index, u32 asset,
                                  u32 page, f32 priority) noexcept {
    const u64 key = (static_cast<u64>(asset) << 32U) | static_cast<u64>(page);
    if (u32* slot = index.find(key); slot != nullptr) {
        PageRequest& request = out.requests[*slot];
        request.priority = priority > request.priority ? priority : request.priority;
        ++request.requests;
        return ok();
    }
    const u32 slot = static_cast<u32>(out.requests.size());
    if (Status pushed = out.requests.push_back(PageRequest{asset, page, priority, 1U}); !pushed) {
        return pushed;
    }
    Expected<u32*, Error> inserted = index.insert(key, slot);
    return inserted ? ok() : Status{make_unexpected(inserted.error())};
}

/// Cull one candidate and emit it. `virtual-geometry` — "Candidate clusters SHALL then be culled
/// by: frustum, **normal cone** backface rejection, screen size, and HZB occlusion."
[[nodiscard]] Status emit_candidate(const InstanceWalk& walk, u32 cluster_index, u32 draw_index,
                                    TraversalResult& out) noexcept {
    const Cluster& cluster = walk.asset.clusters[cluster_index];
    const Cluster& drawn = walk.asset.clusters[draw_index];
    ++out.stats.candidates;

    const Sphere world = to_world(walk.instance, bounds_sphere(drawn.bounds).center,
                                  bounds_sphere(drawn.bounds).radius);
    if (!walk.view.frustum.intersects(world)) {
        ++out.stats.rejected_by_frustum;
        return ok();
    }
    if (walk.view.cone_culling &&
        cone_backfacing(drawn.cone, drawn.bounds.center(), walk.local.camera_position)) {
        ++out.stats.rejected_by_cone;
        return ok();
    }
    if (walk.view.minimum_instance_pixels > 0.0F &&
        screen_diameter(walk.view.projection, world) < 0.5F) {
        // Half a pixel: below it the cluster cannot cover a sample centre. The instance-level test
        // uses `minimum_instance_pixels`; this one is per cluster and much smaller, because a
        // cluster of a large object is legitimately tiny.
        ++out.stats.rejected_by_size;
        return ok();
    }
    // `virtual-geometry` — "Occlusion culling for clusters" specifies a two-pass HZB scheme. This
    // reference has no depth buffer to test against and reports zero rather than pretending: the
    // counter exists, the seam is `nodes_pruned_by_occlusion` and `rejected_by_occlusion`, and
    // README.md says plainly that neither moves at M7.

    VisibleCluster record;
    record.instance = walk.instance_index;
    record.cluster = draw_index;
    record.material = drawn.material + walk.instance.material_offset;
    record.screen_error = project_error(walk.local, cluster.lod_error, cluster.lod_sphere);
    if (Status pushed = out.visible.push_back(record); !pushed) {
        return pushed;
    }
    ++out.stats.visible_clusters;
    out.stats.visible_triangles += drawn.index_count / 3U;
    if (draw_index != cluster_index) {
        ++out.stats.fallback_clusters;
    }
    return ok();
}

}  // namespace

Status traverse_reference(const TraversalInputs& inputs, const TraversalView& view,
                          TraversalResult& out) noexcept {
    out.clear();
    Allocator& allocator = out.visible.allocator();
    HashMap<u64, u32> request_index(allocator);
    Array<GpuNode> stack(allocator);
    Array<u8> emitted(allocator);
    Array<u8> visited(allocator);

    for (u32 instance_index = 0; instance_index < inputs.instances.size(); ++instance_index) {
        const GeometryInstance& instance = inputs.instances[instance_index];
        ++out.stats.instances_tested;
        if ((instance.layer_mask & view.layer_mask) == 0U) {
            ++out.stats.instances_rejected_by_layer;
            continue;
        }
        if (instance.asset >= inputs.assets.size() || inputs.assets[instance.asset] == nullptr) {
            return fail(ErrorCode::OutOfRange, "traverse_reference: an instance names no asset");
        }
        const DecodedAsset& asset = *inputs.assets[instance.asset];
        if (asset.clusters.empty()) {
            continue;
        }

        // --- Instance culling ---------------------------------------------------------------
        //
        // `virtual-geometry`: "Before any hierarchy traversal, whole instances SHALL be culled on
        // the GPU against: frustum, projected screen size, HZB occlusion, and layer or visibility
        // masks."
        const Sphere asset_sphere = bounds_sphere(asset.bounds);
        const Sphere world_sphere = to_world(instance, asset_sphere.center, asset_sphere.radius);
        if (!view.frustum.intersects(world_sphere)) {
            ++out.stats.instances_rejected_by_frustum;
            continue;
        }
        if (view.minimum_instance_pixels > 0.0F &&
            screen_diameter(view.projection, world_sphere) < view.minimum_instance_pixels) {
            ++out.stats.instances_rejected_by_size;
            continue;
        }
        ++out.stats.instances_visible;

        InstanceWalk walk{asset, instance, view, inputs, view.projection, 0.0F, instance_index};
        walk.local.camera_position =
            camera_in_local_space(instance, view.projection.camera_position);
        walk.threshold = view.threshold_pixels * view.secondary_scale * instance.threshold_scale();

        // --- Hierarchy traversal ---------------------------------------------------------------
        stack.clear();
        if (Status resized = emitted.resize(asset.clusters.size()); !resized) {
            return resized;
        }
        if (Status resized = visited.resize(asset.clusters.size()); !resized) {
            return resized;
        }
        for (u32 index = 0; index < asset.clusters.size(); ++index) {
            emitted[index] = 0;
            visited[index] = 0;
        }
        for (u32 index = 0; index < asset.clusters.size(); ++index) {
            if (asset.clusters[index].parent_error < kRootError) {
                continue;
            }
            // A root's own page is the coarsest and is always resident by construction — the
            // serialiser marks the first page resident whatever the budget says — so the ancestor a
            // root carries is itself.
            if (Status pushed = stack.push_back(GpuNode{instance_index, index, index, 0U});
                !pushed) {
                return pushed;
            }
        }

        while (!stack.empty()) {
            const GpuNode node = stack.back();
            stack.pop_back();
            // THE HIERARCHY IS A DAG, NOT A TREE. Every cluster a group produced points at the same
            // child range — the group's members — so a member is reachable from each of its group's
            // parents. Without this the descent would visit some clusters several times, and on a
            // deep hierarchy the duplication compounds: measured on an 88-cluster asset, a
            // tree-shaped descent visited 92 nodes.
            //
            // The first path to reach a node wins, which also fixes which resident ancestor it
            // carries. Any resident ancestor satisfies the requirement — "the nearest resident
            // ancestor" is nearest along the path that reached it — and taking the first makes the
            // choice deterministic rather than dependent on the stack's order.
            if (visited[node.cluster] != 0U) {
                continue;
            }
            visited[node.cluster] = 1U;
            const Cluster& cluster = asset.clusters[node.cluster];
            ++out.stats.nodes_visited;

            // PRUNE ON THE GROUP SPHERE, NOT ON THE CLUSTER'S OWN BOUNDS. `lod_sphere` is the
            // sphere of the group that produced this cluster and therefore contains every cluster
            // beneath it; the cluster's own bounds do not. Pruning on the bounds would prune a
            // subtree whose members reach outside them, which is a hole that appears only at a
            // frustum edge.
            const Sphere subtree =
                to_world(instance, cluster.lod_sphere.center, cluster.lod_sphere.radius);
            if (!view.frustum.intersects(subtree)) {
                ++out.stats.nodes_pruned_by_frustum;
                continue;
            }

            u32 ancestor = node.ancestor;
            const bool resident = page_is_resident(walk, cluster.page);
            if (resident) {
                ancestor = node.cluster;
            }

            if (cluster_too_coarse(cluster, walk.local, walk.threshold) &&
                cluster.child_count > 0) {
                for (u32 slot = 0; slot < cluster.child_count; ++slot) {
                    const u32 child = asset.cluster_children[cluster.first_child + slot];
                    if (Status pushed =
                            stack.push_back(GpuNode{instance_index, child, ancestor, 0U});
                        !pushed) {
                        return pushed;
                    }
                }
                continue;
            }

            // Selected — or as fine as this asset goes, which is the same decision at a leaf.
            u32 draw = node.cluster;
            if (!resident) {
                // `virtual-geometry`: "When a required page is not resident, rendering SHALL use
                // the
                // **nearest resident ancestor** rather than omitting the object", and "An object
                // SHALL never fail to render because streaming has not completed."
                ++out.stats.missing_pages;
                // URGENCY IS THE CLUSTER'S SCREEN SIZE, NOT ITS ERROR. The obvious choice — the
                // projected error of the cluster that wanted the page — is ZERO for every level-0
                // cluster, because a level-0 cluster deviates from the source by nothing. That
                // would give the finest and most-wanted pages the lowest priority, which is the
                // exact inversion of what "Requests SHALL be prioritised" asks for. The screen
                // diameter is what "how much of the frame is waiting for this page" means, and it
                // is divided by the instance's threshold scale so that a critical instance outbids
                // a background one for the same pixels.
                const Sphere wanted =
                    to_world(instance, cluster.lod_sphere.center, cluster.lod_sphere.radius);
                const f32 scale =
                    instance.threshold_scale() > 0.0F ? instance.threshold_scale() : 1.0F;
                const f32 priority = screen_diameter(view.projection, wanted) / scale;
                if (Status noted =
                        note_request(out, request_index, instance.asset, cluster.page, priority);
                    !noted) {
                    return noted;
                }
                draw = ancestor;
            }
            if (emitted[draw] != 0U) {
                continue;  // the substituted ancestor already stands in for a sibling branch
            }
            emitted[draw] = 1U;
            if (Status shown = emit_candidate(walk, node.cluster, draw, out); !shown) {
                return shown;
            }
        }
    }
    return ok();
}

Status pack_clusters(const DecodedAsset& asset, Array<GpuCluster>& out) noexcept {
    if (Status reserved = out.reserve(out.size() + asset.clusters.size()); !reserved) {
        return reserved;
    }
    for (const Cluster& cluster : asset.clusters) {
        GpuCluster record{};
        record.bounds_min[0] = cluster.bounds.min.x;
        record.bounds_min[1] = cluster.bounds.min.y;
        record.bounds_min[2] = cluster.bounds.min.z;
        record.lod_error = cluster.lod_error;
        record.bounds_max[0] = cluster.bounds.max.x;
        record.bounds_max[1] = cluster.bounds.max.y;
        record.bounds_max[2] = cluster.bounds.max.z;
        record.parent_error = cluster.parent_error;
        record.lod_center[0] = cluster.lod_sphere.center.x;
        record.lod_center[1] = cluster.lod_sphere.center.y;
        record.lod_center[2] = cluster.lod_sphere.center.z;
        record.lod_radius = cluster.lod_sphere.radius;
        record.parent_center[0] = cluster.parent_sphere.center.x;
        record.parent_center[1] = cluster.parent_sphere.center.y;
        record.parent_center[2] = cluster.parent_sphere.center.z;
        record.parent_radius = cluster.parent_sphere.radius;
        record.cone_axis[0] = cluster.cone.axis.x;
        record.cone_axis[1] = cluster.cone.axis.y;
        record.cone_axis[2] = cluster.cone.axis.z;
        record.cone_cos = cluster.cone.cos_angle;
        record.material = cluster.material;
        record.page = cluster.page;
        record.first_child = cluster.first_child;
        record.child_count = cluster.child_count;
        if (Status pushed = out.push_back(record); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status pack_instances(Span<const GeometryInstance> instances, Array<GpuInstance>& out) noexcept {
    if (Status reserved = out.reserve(out.size() + instances.size()); !reserved) {
        return reserved;
    }
    for (const GeometryInstance& instance : instances) {
        GpuInstance record{};
        record.translation[0] = instance.translation.x;
        record.translation[1] = instance.translation.y;
        record.translation[2] = instance.translation.z;
        record.scale = instance.scale;
        record.rotation[0] = instance.rotation.x;
        record.rotation[1] = instance.rotation.y;
        record.rotation[2] = instance.rotation.z;
        record.rotation[3] = instance.rotation.w;
        record.asset = instance.asset;
        record.material_offset = instance.material_offset;
        record.threshold_scale = instance.threshold_scale();
        record.layer_mask = instance.layer_mask;
        if (Status pushed = out.push_back(record); !pushed) {
            return pushed;
        }
    }
    return ok();
}

GpuView pack_view(const TraversalView& view, u32 instance_count, u32 cluster_count) noexcept {
    GpuView packed{};
    for (u32 plane = 0; plane < Frustum::kCount; ++plane) {
        packed.frustum[plane][0] = view.frustum.planes[plane].normal.x;
        packed.frustum[plane][1] = view.frustum.planes[plane].normal.y;
        packed.frustum[plane][2] = view.frustum.planes[plane].normal.z;
        packed.frustum[plane][3] = view.frustum.planes[plane].d;
    }
    packed.camera[0] = view.projection.camera_position.x;
    packed.camera[1] = view.projection.camera_position.y;
    packed.camera[2] = view.projection.camera_position.z;
    const f32 tangent = std::tan(view.projection.fov_y_radians * 0.5F);
    packed.pixels_scale =
        (view.projection.viewport_height * 0.5F) / (tangent > 1.0e-6F ? tangent : 1.0e-6F);
    packed.threshold = view.threshold_pixels * view.secondary_scale;
    packed.instance_count = instance_count;
    packed.cluster_count = cluster_count;
    packed.flags = view.cone_culling ? 1U : 0U;
    return packed;
}

}  // namespace cy::rendering::vg
