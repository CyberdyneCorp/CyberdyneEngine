// The interaction field: a deterministic admission, a live query and a decaying trail grid. See
// interaction.h for why the drop is a sort and why the trail is a grid.

#include <cy/foliage/interaction.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <utility>

namespace cy::foliage {

const char* interaction_shape_name(InteractionShape shape) noexcept {
    switch (shape) {
        case InteractionShape::Sphere:
            return "sphere";
        case InteractionShape::Capsule:
            return "capsule";
        case InteractionShape::Box:
            return "box";
    }
    return "unknown";
}

const char* interaction_effect_name(InteractionEffect effect) noexcept {
    switch (effect) {
        case InteractionEffect::Bend:
            return "bend";
        case InteractionEffect::Flatten:
            return "flatten";
        case InteractionEffect::Displace:
            return "displace";
    }
    return "unknown";
}

InteractionField::InteractionField(Allocator& allocator, const InteractionBounds& bounds) noexcept
    : allocator_(&allocator),
      bounds_(bounds),
      pending_(allocator),
      active_(allocator),
      admitted_(allocator),
      trail_(allocator) {
    if (Status sized = trail_.resize(static_cast<usize>(bounds_.trail_bytes())); !sized) {
        // A field whose grid could not be allocated still answers: `sample()` checks the array's
        // own size, so a zero-length trail is a field with live contributors and no memory.
        trail_.clear();
    }
}

Status InteractionField::set_bounds(const InteractionBounds& bounds) noexcept {
    if (!bounds.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "interaction bounds must be positive");
    }
    bounds_ = bounds;
    trail_.clear();
    // CLEARED and not resampled: a trail resampled onto a different grid is a trail that moved, and
    // a budget lever that moved the world's tracks would be worse than one that forgot them.
    return trail_.resize(static_cast<usize>(bounds_.trail_bytes()));
}

bool InteractionField::cell_of(const world::WorldVec3d& at, i32& cx, i32& cz) const noexcept {
    const f64 cell = static_cast<f64>(bounds_.trail_cell_metres());
    if (!(cell > 0.0)) {
        return false;
    }
    const auto absolute_x = static_cast<i64>(std::floor(at.x / cell));
    const auto absolute_z = static_cast<i64>(std::floor(at.z / cell));
    const i64 local_x = absolute_x - origin_cell_x_;
    const i64 local_z = absolute_z - origin_cell_z_;
    if (local_x < 0 || local_z < 0 || std::cmp_greater_equal(local_x, bounds_.trail_cells) ||
        std::cmp_greater_equal(local_z, bounds_.trail_cells)) {
        return false;
    }
    cx = static_cast<i32>(local_x);
    cz = static_cast<i32>(local_z);
    return true;
}

Status InteractionField::recentre(const world::WorldVec3d& centre) noexcept {
    const f64 cell = static_cast<f64>(bounds_.trail_cell_metres());
    if (!(cell > 0.0) || trail_.empty()) {
        centre_ = centre;
        return ok();
    }
    const auto half = static_cast<i64>(bounds_.trail_cells / 2);
    const i64 wanted_x = static_cast<i64>(std::floor(centre.x / cell)) - half;
    const i64 wanted_z = static_cast<i64>(std::floor(centre.z / cell)) - half;
    const i64 shift_x = wanted_x - origin_cell_x_;
    const i64 shift_z = wanted_z - origin_cell_z_;
    centre_ = centre;
    if (shift_x == 0 && shift_z == 0) {
        return ok();
    }
    // Shifted in WHOLE CELLS, so a track stays exactly where it was laid. Cells that scroll in are
    // cleared; a bulk clear when the shift exceeds the grid, because every cell would scroll in.
    const auto side = static_cast<i64>(bounds_.trail_cells);
    if (shift_x <= -side || shift_x >= side || shift_z <= -side || shift_z >= side) {
        for (u8& value : trail_) {
            value = 0;
        }
        origin_cell_x_ = wanted_x;
        origin_cell_z_ = wanted_z;
        return ok();
    }
    // Copied into a scratch array rather than in place: an in-place shift has to walk in the right
    // direction per axis, and getting it wrong smears the trail in a way that looks like motion
    // blur and is a bug.
    Array<u8> shifted(*allocator_);
    if (Status sized = shifted.resize(trail_.size()); !sized) {
        return sized;
    }
    for (i64 z = 0; z < side; ++z) {
        for (i64 x = 0; x < side; ++x) {
            const i64 source_x = x + shift_x;
            const i64 source_z = z + shift_z;
            const auto destination = static_cast<usize>((z * side) + x);
            if (source_x < 0 || source_z < 0 || source_x >= side || source_z >= side) {
                shifted[destination] = 0;
                continue;
            }
            shifted[destination] = trail_[static_cast<usize>((source_z * side) + source_x)];
        }
    }
    trail_ = static_cast<Array<u8>&&>(shifted);
    origin_cell_x_ = wanted_x;
    origin_cell_z_ = wanted_z;
    return ok();
}

Status InteractionField::register_primitive(const InteractionPrimitive& primitive) noexcept {
    return pending_.push_back(primitive);
}

namespace {

/// The admission order. Priority first, then the CALLER's own identity, then the quantised
/// position — a total order over values that do not depend on when anything was registered, which
/// is the whole of "dropped deterministically".
[[nodiscard]] bool admits_before(const InteractionPrimitive& a,
                                 const InteractionPrimitive& b) noexcept {
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    if (a.source != b.source) {
        return a.source < b.source;
    }
    const auto ax = static_cast<i64>(a.position.x * 1024.0);
    const auto bx = static_cast<i64>(b.position.x * 1024.0);
    if (ax != bx) {
        return ax < bx;
    }
    const auto az = static_cast<i64>(a.position.z * 1024.0);
    const auto bz = static_cast<i64>(b.position.z * 1024.0);
    return az < bz;
}

void sort_by_admission(Span<InteractionPrimitive> items) noexcept {
    for (usize index = 1; index < items.size(); ++index) {
        const InteractionPrimitive key = items[index];
        usize hole = index;
        while (hole > 0 && admits_before(key, items[hole - 1])) {
            items[hole] = items[hole - 1];
            --hole;
        }
        items[hole] = key;
    }
}

/// Distance from a position to a primitive's surface, in metres. Negative inside.
[[nodiscard]] f64 distance_to(const InteractionPrimitive& primitive,
                              const world::WorldVec3d& at) noexcept {
    switch (primitive.shape) {
        case InteractionShape::Sphere: {
            const f64 dx = at.x - primitive.position.x;
            const f64 dz = at.z - primitive.position.z;
            return std::sqrt((dx * dx) + (dz * dz)) - static_cast<f64>(primitive.radius_metres);
        }
        case InteractionShape::Capsule: {
            // Closest point on the segment, in the horizontal plane. Foliage bends horizontally, so
            // the vertical component of a capsule is deliberately not part of the distance.
            const f64 ax = primitive.position.x;
            const f64 az = primitive.position.z;
            const f64 bx = ax + static_cast<f64>(primitive.extent.x);
            const f64 bz = az + static_cast<f64>(primitive.extent.z);
            const f64 vx = bx - ax;
            const f64 vz = bz - az;
            const f64 length_squared = (vx * vx) + (vz * vz);
            f64 t = 0.0;
            if (length_squared > 0.0) {
                t = math::clamp((((at.x - ax) * vx) + ((at.z - az) * vz)) / length_squared, 0.0,
                                1.0);
            }
            const f64 dx = at.x - (ax + (vx * t));
            const f64 dz = at.z - (az + (vz * t));
            return std::sqrt((dx * dx) + (dz * dz)) - static_cast<f64>(primitive.radius_metres);
        }
        case InteractionShape::Box: {
            const f64 dx =
                std::abs(at.x - primitive.position.x) - static_cast<f64>(primitive.extent.x);
            const f64 dz =
                std::abs(at.z - primitive.position.z) - static_cast<f64>(primitive.extent.z);
            const f64 ox = dx > 0.0 ? dx : 0.0;
            const f64 oz = dz > 0.0 ? dz : 0.0;
            const f64 outside = std::sqrt((ox * ox) + (oz * oz));
            const f64 inside = math::min(math::max(dx, dz), 0.0);
            return outside + inside;
        }
    }
    return 1e9;
}

}  // namespace

Status InteractionField::deposit_trail(const InteractionPrimitive& primitive) noexcept {
    if (trail_.empty()) {
        return ok();
    }
    const f32 cell_metres = bounds_.trail_cell_metres();
    const auto side = static_cast<i64>(bounds_.trail_cells);
    const f64 reach =
        static_cast<f64>(primitive.radius_metres) +
        static_cast<f64>(math::max(std::abs(primitive.extent.x), std::abs(primitive.extent.z)));
    const auto span = static_cast<i64>(std::ceil(reach / static_cast<f64>(cell_metres))) + 1;
    i32 cx = 0;
    i32 cz = 0;
    if (!cell_of(primitive.position, cx, cz)) {
        // The primitive's own centre is outside the grid; its footprint may still reach in, so the
        // walk is over the local rectangle rather than abandoned.
        const f64 cell = static_cast<f64>(cell_metres);
        cx = static_cast<i32>(static_cast<i64>(std::floor(primitive.position.x / cell)) -
                              origin_cell_x_);
        cz = static_cast<i32>(static_cast<i64>(std::floor(primitive.position.z / cell)) -
                              origin_cell_z_);
    }
    for (i64 z = cz - span; z <= cz + span; ++z) {
        if (z < 0 || z >= side) {
            continue;
        }
        for (i64 x = cx - span; x <= cx + span; ++x) {
            if (x < 0 || x >= side) {
                continue;
            }
            const world::WorldVec3d at{
                (static_cast<f64>(origin_cell_x_ + x) * static_cast<f64>(cell_metres)) +
                    (static_cast<f64>(cell_metres) * 0.5),
                primitive.position.y,
                (static_cast<f64>(origin_cell_z_ + z) * static_cast<f64>(cell_metres)) +
                    (static_cast<f64>(cell_metres) * 0.5)};
            const f64 distance = distance_to(primitive, at);
            if (distance > 0.0) {
                continue;
            }
            const auto slot = static_cast<usize>((z * side) + x);
            const auto strength =
                static_cast<u8>(math::clamp(primitive.strength, 0.0F, 1.0F) * 255.0F);
            // Max rather than sum: a vehicle passing twice over one patch leaves a track, not a
            // hole, and a sum would saturate the byte on the second pass.
            trail_[slot] = math::max(trail_[slot], strength);
        }
    }
    return ok();
}

Expected<InteractionReport, Error> InteractionField::resolve(f32 seconds) noexcept {
    InteractionReport report;
    report.registered = static_cast<u32>(pending_.size());

    // Age the contributors that declared a lifetime, and drop the expired ones.
    for (usize index = active_.size(); index > 0; --index) {
        Active& entry = active_[index - 1];
        entry.remaining -= seconds;
        if (entry.remaining <= 0.0F) {
            ++report.dropped_expired;
            active_.remove_unordered(index - 1);
        }
    }
    for (const InteractionPrimitive& primitive : pending_) {
        if (primitive.lifetime_seconds > 0.0F) {
            Active entry;
            entry.primitive = primitive;
            entry.remaining = primitive.lifetime_seconds;
            if (Status pushed = active_.push_back(entry); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    // The candidate set for this frame: this frame's registrations plus the lingering ones.
    Array<InteractionPrimitive> candidates(*allocator_);
    if (Status appended = candidates.append(pending_.span()); !appended) {
        return make_unexpected(appended.error());
    }
    for (const Active& entry : active_) {
        if (entry.primitive.lifetime_seconds > 0.0F &&
            entry.remaining < entry.primitive.lifetime_seconds) {
            if (Status pushed = candidates.push_back(entry.primitive); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    pending_.clear();

    // The extent test first, so a distant contributor does not occupy a slot the bound would have
    // given to a near one.
    const f64 extent = static_cast<f64>(bounds_.extent_metres);
    admitted_.clear();
    Array<InteractionPrimitive> inside(*allocator_);
    for (const InteractionPrimitive& primitive : candidates) {
        const f64 dx = std::abs(primitive.position.x - centre_.x);
        const f64 dz = std::abs(primitive.position.z - centre_.z);
        const f64 reach = static_cast<f64>(primitive.radius_metres);
        if (dx - reach > extent || dz - reach > extent) {
            ++report.dropped_extent;
            continue;
        }
        if (Status pushed = inside.push_back(primitive); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    sort_by_admission(inside.span());
    for (const InteractionPrimitive& primitive : inside) {
        if (admitted_.size() >= bounds_.max_contributors) {
            ++report.dropped_priority;
            continue;
        }
        if (Status pushed = admitted_.push_back(primitive); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (primitive.deposits_trail) {
            if (Status deposited = deposit_trail(primitive); !deposited) {
                return make_unexpected(deposited.error());
            }
        }
    }
    report.admitted = static_cast<u32>(admitted_.size());

    // Exponential decay over the DECLARED lifetime. `foliage` — "a decaying contribution with a
    // declared lifetime", and the declaration is `InteractionBounds::trail_lifetime_seconds`.
    if (!trail_.empty() && seconds > 0.0F && bounds_.trail_lifetime_seconds > 0.0F) {
        const f32 factor = std::exp(-seconds / bounds_.trail_lifetime_seconds);
        for (u8& value : trail_) {
            value = static_cast<u8>(static_cast<f32>(value) * factor);
        }
    }
    return report;
}

InteractionSample InteractionField::sample(const world::WorldVec3d& at) const noexcept {
    InteractionSample result;
    for (const InteractionPrimitive& primitive : admitted_) {
        const f64 distance = distance_to(primitive, at);
        if (distance > 0.0) {
            continue;
        }
        ++result.contributors;
        const f64 depth = -distance;
        const f64 reach = static_cast<f64>(primitive.radius_metres);
        const f32 falloff = reach > 0.0 ? static_cast<f32>(math::min(depth / reach, 1.0)) : 1.0F;
        const f32 strength = primitive.strength * falloff;
        switch (primitive.effect) {
            case InteractionEffect::Flatten:
                result.flatten = math::max(result.flatten, strength);
                break;
            case InteractionEffect::Bend:
            case InteractionEffect::Displace: {
                // Away from the primitive's centre. A zero-length direction — a query exactly on
                // the centre — pushes along +X rather than producing a NaN.
                const f64 dx = at.x - primitive.position.x;
                const f64 dz = at.z - primitive.position.z;
                const f64 length = std::sqrt((dx * dx) + (dz * dz));
                const f32 ux = length > 1e-6 ? static_cast<f32>(dx / length) : 1.0F;
                const f32 uz = length > 1e-6 ? static_cast<f32>(dz / length) : 0.0F;
                const f32 scale = primitive.effect == InteractionEffect::Displace ? 1.0F : 0.6F;
                result.offset.x += ux * strength * static_cast<f32>(reach) * scale;
                result.offset.z += uz * strength * static_cast<f32>(reach) * scale;
                break;
            }
        }
    }
    i32 cx = 0;
    i32 cz = 0;
    if (!trail_.empty() && cell_of(at, cx, cz)) {
        const auto side = static_cast<i64>(bounds_.trail_cells);
        result.trail = static_cast<f32>(trail_[static_cast<usize>((cz * side) + cx)]) / 255.0F;
        result.flatten = math::max(result.flatten, result.trail);
    }
    return result;
}

u64 InteractionField::bytes() const noexcept {
    return static_cast<u64>(trail_.size()) + (static_cast<u64>(active_.size()) * sizeof(Active)) +
           (static_cast<u64>(admitted_.size()) * sizeof(InteractionPrimitive));
}

f32 InteractionField::peak_trail() const noexcept {
    u8 peak = 0;
    for (u8 value : trail_) {
        peak = math::max(peak, value);
    }
    return static_cast<f32>(peak) / 255.0F;
}

}  // namespace cy::foliage
