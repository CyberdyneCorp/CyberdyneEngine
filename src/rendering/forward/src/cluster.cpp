#include <cy/rendering/forward/cluster.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// Squared distance from a point to a box, zero inside it. The sphere test's whole content.
[[nodiscard]] f32 distance_squared_to(const Aabb& box, Vec3 point) noexcept {
    const Vec3 clamped{math::clamp(point.x, box.min.x, box.max.x),
                       math::clamp(point.y, box.min.y, box.max.y),
                       math::clamp(point.z, box.min.z, box.max.z)};
    return length_squared(point - clamped);
}

/// Whether a cone may reach a sphere. The standard conservative test: reject when the sphere's
/// centre is outside the cone widened by the sphere's radius.
[[nodiscard]] bool cone_intersects_sphere(Vec3 apex, Vec3 axis, f32 cone_cos, f32 range,
                                          Vec3 center, f32 radius) noexcept {
    const Vec3 offset = center - apex;
    const f32 distance = length(offset);
    if (distance > range + radius) {
        return false;
    }
    if (distance <= radius) {
        // The sphere contains the apex: the cone certainly reaches it, whatever the angle. This is
        // also the camera-inside-a-light case for a spot, and it must not be rejected by the angle
        // test below, which divides by the distance.
        return true;
    }
    const f32 cone_sin = std::sqrt(math::max(1.0F - (cone_cos * cone_cos), 0.0F));
    const f32 along = dot(offset, axis);
    const f32 perpendicular = std::sqrt(math::max((distance * distance) - (along * along), 0.0F));
    // The sphere's centre must be inside the cone widened by `radius`, measured perpendicular to
    // the axis after the widening has been projected onto it.
    const f32 widened = (cone_cos * perpendicular) - (cone_sin * along);
    return widened <= radius;
}

/// One element against one cluster.
[[nodiscard]] bool element_touches(const ClusterElement& element, const Aabb& bounds) noexcept {
    if (distance_squared_to(bounds, element.view_position) > element.radius * element.radius) {
        return false;
    }
    if (element.cone_cos >= 1.0F) {
        return true;
    }
    // A cone is tested against the cluster's bounding SPHERE rather than its box: the box test for
    // a cone is several times the arithmetic to reject a few more clusters, and both are
    // conservative.
    const Vec3 center = bounds.center();
    const f32 radius = length(bounds.half_extents());
    return cone_intersects_sphere(element.view_position, element.view_direction, element.cone_cos,
                                  element.radius, center, radius);
}

/// A bounded, nearest-kept candidate list for one cluster and one type.
///
/// Insertion keeps at most `capacity` entries; when full, the furthest is replaced if the newcomer
/// is nearer. That is "the excess SHALL be dropped deterministically (nearest kept)" — and it is
/// deterministic because distance is a property of the geometry and ties keep the incumbent, so the
/// outcome does not depend on the order elements happened to arrive in except through their
/// indices, which the final sort removes.
class NearestKeptList {
public:
    NearestKeptList(u32* storage, f32* distances, u32 capacity) noexcept
        : storage_(storage), distances_(distances), capacity_(capacity) {}

    /// Returns true when something was dropped — the newcomer, or the incumbent it displaced.
    /// Either way one assignment did not survive, which is what the overflow counter counts.
    bool insert(u32 payload, f32 distance_squared) noexcept {
        if (size_ < capacity_) {
            storage_[size_] = payload;
            distances_[size_] = distance_squared;
            ++size_;
            return false;
        }
        u32 furthest = 0;
        for (u32 index = 1; index < size_; ++index) {
            if (distances_[index] > distances_[furthest]) {
                furthest = index;
            }
        }
        // A tie keeps the incumbent, which is what makes the outcome independent of arrival order.
        if (distance_squared < distances_[furthest]) {
            storage_[furthest] = payload;
            distances_[furthest] = distance_squared;
        }
        return true;
    }

    /// Ascending payload order. An insertion sort because the list is at most a few dozen entries
    /// and the whole point is that the output is identical between two runs.
    void sort() noexcept {
        for (u32 index = 1; index < size_; ++index) {
            const u32 value = storage_[index];
            u32 position = index;
            while (position > 0 && storage_[position - 1] > value) {
                storage_[position] = storage_[position - 1];
                --position;
            }
            storage_[position] = value;
        }
    }

    [[nodiscard]] u32 size() const noexcept { return size_; }

private:
    u32* storage_ = nullptr;
    f32* distances_ = nullptr;
    u32 capacity_ = 0;
    u32 size_ = 0;
};

/// The most elements one cluster may hold of one type. A ceiling on the scratch the assignment
/// allocates on the stack; a configuration above it is refused rather than silently truncated.
constexpr u32 kMaxElementsPerClusterLimit = 256;

}  // namespace

Expected<ClusterGrid, Error> make_cluster_grid(const ClusterGridConfig& config, u32 view_width,
                                               u32 view_height, f32 near_plane,
                                               f32 far_plane) noexcept {
    if (config.tile_size == 0 || config.slice_count == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "cluster grid: tile size and slice count must be "
                    "non-zero");
    }
    if (view_width == 0 || view_height == 0) {
        return fail(ErrorCode::InvalidArgument, "cluster grid: the viewport is empty");
    }
    if (!(near_plane > 0.0F) || !(far_plane > near_plane) || !std::isfinite(far_plane)) {
        return fail(ErrorCode::InvalidArgument,
                    "cluster grid: 0 < near < far, and far must be finite — see the header for why "
                    "an infinite projection supplies a content extent here");
    }
    if (config.max_elements_per_cluster == 0 ||
        config.max_elements_per_cluster > kMaxElementsPerClusterLimit) {
        return fail(ErrorCode::InvalidArgument,
                    "cluster grid: max_elements_per_cluster must be 1..256");
    }

    ClusterGrid grid;
    // Ceiling division, so a viewport that is not a multiple of the tile is covered rather than
    // clipped — the tile at the edge is partly outside the view and costs nothing.
    grid.dimensions[0] = (view_width + config.tile_size - 1) / config.tile_size;
    grid.dimensions[1] = (view_height + config.tile_size - 1) / config.tile_size;
    grid.dimensions[2] = config.slice_count;
    grid.max_elements_per_cluster = config.max_elements_per_cluster;
    grid.near_plane = near_plane;
    grid.far_plane = far_plane;

    const f32 log_ratio = std::log2(far_plane / near_plane);
    const auto slices = static_cast<f32>(config.slice_count);
    grid.slice_scale = slices / log_ratio;
    grid.slice_bias = -(slices * std::log2(near_plane)) / log_ratio;
    return grid;
}

u32 cluster_slice_of(const ClusterGrid& grid, f32 view_depth) noexcept {
    if (grid.dimensions[2] == 0) {
        return 0;
    }
    const f32 depth = math::max(view_depth, grid.near_plane);
    const f32 slice = (std::log2(depth) * grid.slice_scale) + grid.slice_bias;
    const f32 clamped = math::clamp(slice, 0.0F, static_cast<f32>(grid.dimensions[2] - 1));
    return static_cast<u32>(clamped);
}

Aabb cluster_bounds(const ClusterGrid& grid, u32 x, u32 y, u32 slice, f32 tan_half_fov_y,
                    f32 aspect) noexcept {
    // The slice's depth range, inverted from the slice mapping: z = near · 2^(slice / scale).
    const f32 scale = grid.slice_scale != 0.0F ? grid.slice_scale : 1.0F;
    const f32 near_depth = grid.near_plane * std::exp2(static_cast<f32>(slice) / scale);
    const f32 far_depth = grid.near_plane * std::exp2(static_cast<f32>(slice + 1) / scale);

    // The tile's extent in normalised device coordinates, then as a tangent per unit depth. The
    // tile's corners span a frustum, so the box that contains it is the union of the two rectangles
    // at the slice's two depths.
    const f32 tile_min_x =
        ((static_cast<f32>(x) / static_cast<f32>(grid.dimensions[0])) * 2.0F) - 1.0F;
    const f32 tile_max_x =
        ((static_cast<f32>(x + 1) / static_cast<f32>(grid.dimensions[0])) * 2.0F) - 1.0F;
    const f32 tile_min_y =
        ((static_cast<f32>(y) / static_cast<f32>(grid.dimensions[1])) * 2.0F) - 1.0F;
    const f32 tile_max_y =
        ((static_cast<f32>(y + 1) / static_cast<f32>(grid.dimensions[1])) * 2.0F) - 1.0F;

    const f32 tan_x = tan_half_fov_y * aspect;
    const f32 min_x = math::min(tile_min_x * tan_x * near_depth, tile_min_x * tan_x * far_depth);
    const f32 max_x = math::max(tile_max_x * tan_x * near_depth, tile_max_x * tan_x * far_depth);
    const f32 min_y = math::min(tile_min_y * tan_half_fov_y * near_depth,
                                tile_min_y * tan_half_fov_y * far_depth);
    const f32 max_y = math::max(tile_max_y * tan_half_fov_y * near_depth,
                                tile_max_y * tan_half_fov_y * far_depth);

    // View space looks down −Z, so the near depth is the LARGER z. Getting this the wrong way round
    // produces an empty box and a frame with no lights, which is why it is written out.
    return Aabb::from_min_max(Vec3{math::min(min_x, max_x), math::min(min_y, max_y), -far_depth},
                              Vec3{math::max(min_x, max_x), math::max(min_y, max_y), -near_depth});
}

bool element_is_cone(f32 outer_half_angle_radians) noexcept {
    return outer_half_angle_radians <= kSpotConeThresholdRadians;
}

ClusterAssignment::ClusterAssignment(Allocator& allocator) noexcept
    : headers(allocator), indices(allocator) {}

void ClusterAssignment::clear() noexcept {
    headers.clear();
    indices.clear();
    stats = ClusterStatistics{};
}

namespace {

/// Scratch for one cluster's candidate list, handed down rather than allocated per cluster.
struct AssignmentScratch {
    u32* payloads = nullptr;
    f32* distances = nullptr;
    u32 capacity = 0;
};

/// Gather the elements of one type that touch one cluster, keeping the nearest. Returns how many
/// assignments were dropped because the cluster was full.
[[nodiscard]] u32 collect_type(Span<const ClusterElement> elements, u32 type, u32 view_layer_mask,
                               const Aabb& bounds, Vec3 center, NearestKeptList& kept) noexcept {
    u32 dropped = 0;
    for (const ClusterElement& element : elements) {
        if (static_cast<u32>(element.type) != type) {
            continue;
        }
        // Light channels: one AND, during assignment, and never per pixel.
        if ((element.layer_mask & view_layer_mask) == 0U) {
            continue;
        }
        if (!element_touches(element, bounds)) {
            continue;
        }
        if (kept.insert(element.payload_index, length_squared(element.view_position - center))) {
            ++dropped;
        }
    }
    kept.sort();
    return dropped;
}

/// One cluster: a header and an index run per element type.
[[nodiscard]] Status assign_cluster(Span<const ClusterElement> elements, u32 view_layer_mask,
                                    const Aabb& bounds, u32 cluster, AssignmentScratch scratch,
                                    ClusterAssignment& out) noexcept {
    const Vec3 center = bounds.center();
    u32 total = 0;
    for (u32 type = 0; type < kClusterElementTypeCount; ++type) {
        NearestKeptList kept(scratch.payloads, scratch.distances, scratch.capacity);
        out.stats.overflow += collect_type(elements, type, view_layer_mask, bounds, center, kept);

        ClusterHeader& header =
            out.headers[(static_cast<usize>(cluster) * kClusterElementTypeCount) + type];
        header.offset = static_cast<u32>(out.indices.size());
        header.count = kept.size();
        for (u32 index = 0; index < kept.size(); ++index) {
            if (Status pushed = out.indices.push_back(scratch.payloads[index]); !pushed) {
                return pushed;
            }
        }
        total += kept.size();
    }
    out.stats.max_elements = math::max(out.stats.max_elements, total);
    out.stats.non_empty_clusters += total != 0 ? 1U : 0U;
    return ok();
}

}  // namespace

Status assign_clusters(const ClusterGrid& grid, Span<const ClusterElement> elements,
                       u32 view_layer_mask, f32 tan_half_fov_y, f32 aspect,
                       ClusterAssignment& out) noexcept {
    const u32 clusters = grid.cluster_count();
    if (clusters == 0) {
        return fail(ErrorCode::InvalidArgument, "assign_clusters: the grid is empty");
    }
    if (grid.max_elements_per_cluster > kMaxElementsPerClusterLimit) {
        return fail(ErrorCode::InvalidArgument,
                    "assign_clusters: max_elements_per_cluster is above the 256 the scratch is "
                    "sized for");
    }

    out.clear();
    if (Status sized = out.headers.resize(static_cast<usize>(clusters) * kClusterElementTypeCount);
        !sized) {
        return sized;
    }
    out.stats.clusters = clusters;
    out.stats.elements = static_cast<u32>(elements.size());

    // Zero-initialised so a static analyser can see that nothing reads an uninitialised slot;
    // `NearestKeptList` only ever reads what it has written, but proving that costs it a flow it
    // does not have.
    u32 payloads[kMaxElementsPerClusterLimit] = {};
    f32 distances[kMaxElementsPerClusterLimit] = {};
    const AssignmentScratch scratch{payloads, distances, grid.max_elements_per_cluster};

    for (u32 slice = 0; slice < grid.dimensions[2]; ++slice) {
        for (u32 y = 0; y < grid.dimensions[1]; ++y) {
            for (u32 x = 0; x < grid.dimensions[0]; ++x) {
                const Aabb bounds = cluster_bounds(grid, x, y, slice, tan_half_fov_y, aspect);
                if (Status assigned =
                        assign_cluster(elements, view_layer_mask, bounds,
                                       cluster_index_of(grid, x, y, slice), scratch, out);
                    !assigned) {
                    return assigned;
                }
            }
        }
    }

    out.stats.average_elements = static_cast<f32>(out.indices.size()) / static_cast<f32>(clusters);
    return ok();
}

}  // namespace cy::rendering
