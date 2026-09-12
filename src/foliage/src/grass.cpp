// Ground cover: patch descriptions in, blades out, nothing stored between. See grass.h for why the
// ratio is the requirement and why the expansion is written on the CPU.

#include <cy/foliage/grass.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::foliage {

u32 GrassPatch::full_blade_count() const noexcept {
    const f32 edge = edge_metres();
    const f32 area = edge * edge;
    return static_cast<u32>(area * static_cast<f32>(density));
}

GrassField::GrassField(Allocator& allocator) noexcept : patches_(allocator) {}

Status GrassField::add(ClusterId cluster, const ClusterBounds& bounds,
                       const GrassPatch& patch) noexcept {
    Entry entry;
    entry.cluster = cluster;
    entry.bounds = bounds;
    entry.patch = patch;
    return patches_.push_back(entry);
}

u64 GrassField::bytes() const noexcept {
    // The PATCH bytes, not the entry bytes: the bounds and the cluster identity are bookkeeping the
    // runtime holds, and what "persistent storage for ground cover" means is the descriptions.
    return static_cast<u64>(patches_.size()) * sizeof(GrassPatch);
}

u32 GrassField::evict(ClusterId cluster) noexcept {
    u32 removed = 0;
    for (usize index = patches_.size(); index > 0; --index) {
        if (patches_[index - 1].cluster == cluster) {
            patches_.remove_unordered(index - 1);
            ++removed;
        }
    }
    return removed;
}

namespace {

/// The stream one patch expands in. Keyed by the patch's own stored seed, which the cooker derived
/// from the world seed and the patch's position — so a blade is a function of stable identifiers
/// and a meadow expands identically on every machine and in every frame.
[[nodiscard]] determinism::RandomStream patch_stream(const GrassPatch& patch) noexcept {
    const determinism::StreamId stream =
        determinism::substream(determinism::stream_id(kGrassStream), patch.seed);
    // Presentation: a blade's position is not authoritative state. It still has to be REPRODUCIBLE,
    // and it is — the purpose declares what the value is FOR, not whether it is derived.
    return {patch.seed, stream, determinism::StreamPurpose::Presentation};
}

enum : u64 {
    kBladeX = 0,
    kBladeZ,
    kBladeHeight,
    kBladeYaw,
    kBladeLean,
    kBladePhase,
    kDrawsPerBlade,
};

}  // namespace

u32 GrassField::expand_patch(const GrassPatch& patch, const ClusterBounds& bounds,
                             f32 density_scale, u32 max_blades, const InteractionField* interaction,
                             Array<GrassBlade>& out) noexcept {
    const f32 edge = patch.edge_metres();
    const u32 full = patch.full_blade_count();
    const auto wanted =
        static_cast<u32>(static_cast<f32>(full) * math::clamp(density_scale, 0.0F, 1.0F));
    const u32 count = math::min(wanted, max_blades);
    if (count == 0) {
        return 0;
    }
    // The patch's origin is quantised against its CLUSTER, exactly as an instance is, so a patch
    // and the trees around it are addressed in one coordinate system.
    FoliageInstance anchor;
    anchor.qx = patch.qx;
    anchor.qz = patch.qz;
    const world::WorldVec3d origin = bounds.decode(anchor);

    const determinism::RandomStream draws = patch_stream(patch);
    const f32 base_yaw = static_cast<f32>(patch.orientation) * (math::kTwoPi / 256.0F);
    const f32 spread = static_cast<f32>(patch.spread_degrees) * math::kDegToRad;
    u32 written = 0;
    for (u32 index = 0; index < count; ++index) {
        const u64 base = static_cast<u64>(index) * kDrawsPerBlade;
        GrassBlade blade;
        const f32 u = draws.unit_float(generation_point(), index, base + kBladeX);
        const f32 v = draws.unit_float(generation_point(), index, base + kBladeZ);
        blade.position.x = origin.x + static_cast<f64>(u * edge);
        blade.position.z = origin.z + static_cast<f64>(v * edge);
        // The patch stores the ground plane rather than querying terrain per blade: a terrain query
        // per blade would be a terrain query ten million times a frame, and the plane is exact for
        // a patch a few metres across.
        blade.position.y = static_cast<f64>(patch.base_height + (patch.height_dx * (u * edge)) +
                                            (patch.height_dz * (v * edge)));
        const f32 height_draw =
            (draws.unit_float(generation_point(), index, base + kBladeHeight) * 2.0F) - 1.0F;
        blade.height_metres = (static_cast<f32>(patch.height_cm) +
                               (height_draw * static_cast<f32>(patch.height_variance_cm))) *
                              0.01F;
        blade.yaw =
            base_yaw +
            ((draws.unit_float(generation_point(), index, base + kBladeYaw) - 0.5F) * spread);
        blade.lean = draws.unit_float(generation_point(), index, base + kBladeLean) * 0.2F;
        blade.phase = draws.unit_float(generation_point(), index, base + kBladePhase);
        if (interaction != nullptr) {
            // The SAME `InteractionField::sample()` an instanced plant calls. "Ground cover SHALL
            // respond to ... the interaction field LIKE OTHER FOLIAGE" is one function, not two.
            blade.flatten = interaction->sample(blade.position).flatten;
        }
        if (Status pushed = out.push_back(blade); !pushed) {
            return written;
        }
        ++written;
    }
    return written;
}

namespace {

[[nodiscard]] f64 planar_distance(const world::WorldVec3d& a, const world::WorldVec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

[[nodiscard]] bool patch_visible(const CullView& view, const world::WorldVec3d& centre,
                                 f64 radius) noexcept {
    for (u32 plane = 0; plane < view.plane_count && plane < 6; ++plane) {
        const f64 distance = (view.planes[plane][0] * centre.x) +
                             (view.planes[plane][1] * centre.y) +
                             (view.planes[plane][2] * centre.z) + view.planes[plane][3];
        if (distance < -radius) {
            return false;
        }
    }
    return true;
}

/// Order patches by distance so that the ceiling drops the FAR ones. Truncating the array instead
/// would leave a straight edge across the meadow at whatever distance the budget ran out.
void sort_by_distance(Span<u32> order, Span<const f32> distances) noexcept {
    for (usize index = 1; index < order.size(); ++index) {
        const u32 key = order[index];
        const f32 key_distance = distances[key];
        usize hole = index;
        while (hole > 0 && distances[order[hole - 1]] > key_distance) {
            order[hole] = order[hole - 1];
            --hole;
        }
        order[hole] = key;
    }
}

}  // namespace

Expected<GrassReport, Error> GrassField::expand(const GrassBudget& budget, const CullView& view,
                                                const InteractionField* interaction,
                                                Array<GrassBlade>& out) const noexcept {
    if (!budget.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "grass budget must be positive");
    }
    GrassReport report;
    report.patches_considered = static_cast<u32>(patches_.size());
    report.bytes_stored = bytes();

    Allocator& allocator = out.allocator();
    Array<u32> visible(allocator);
    Array<f32> distances(allocator);
    if (Status sized = distances.resize(patches_.size()); !sized) {
        return make_unexpected(sized.error());
    }

    for (usize index = 0; index < patches_.size(); ++index) {
        const Entry& entry = patches_[index];
        FoliageInstance anchor;
        anchor.qx = entry.patch.qx;
        anchor.qz = entry.patch.qz;
        const world::WorldVec3d origin = entry.bounds.decode(anchor);
        const f32 edge = entry.patch.edge_metres();
        const world::WorldVec3d centre{origin.x + (static_cast<f64>(edge) * 0.5),
                                       static_cast<f64>(entry.patch.base_height),
                                       origin.z + (static_cast<f64>(edge) * 0.5)};
        const f64 radius = static_cast<f64>(edge) * 0.71;
        const f64 distance = planar_distance(view.eye, centre);
        distances[index] = static_cast<f32>(distance);
        if (distance - radius > static_cast<f64>(budget.distance_metres)) {
            ++report.patches_beyond_distance;
            continue;
        }
        if (!patch_visible(view, centre, radius)) {
            // "Blades SHALL be expanded ONLY WHERE VISIBLE": the patch's own bounds against the
            // view, before any blade is derived.
            ++report.patches_outside_view;
            continue;
        }
        if (Status pushed = visible.push_back(static_cast<u32>(index)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    sort_by_distance(visible.span(), distances.span());

    u64 remaining = budget.max_blades;
    for (u32 index : visible) {
        const Entry& entry = patches_[index];
        const auto full = static_cast<u64>(entry.patch.full_blade_count());
        report.blades_at_full_density += full;
        const auto wanted = static_cast<u32>(static_cast<f32>(full) *
                                             math::clamp(budget.density_scale, 0.0F, 1.0F));
        if (wanted < budget.min_blades_per_patch) {
            // Expanding four blades costs a draw's overhead for four blades.
            ++report.patches_below_minimum;
            continue;
        }
        if (remaining == 0) {
            ++report.patches_over_budget;
            continue;
        }
        const auto allowed = static_cast<u32>(math::min(static_cast<u64>(wanted), remaining));
        const u32 written = expand_patch(entry.patch, entry.bounds, budget.density_scale, allowed,
                                         interaction, out);
        if (written < wanted) {
            ++report.patches_over_budget;
        }
        report.blades_expanded += written;
        remaining -= math::min(static_cast<u64>(written), remaining);
        ++report.patches_expanded;
    }
    return report;
}

}  // namespace cy::foliage
