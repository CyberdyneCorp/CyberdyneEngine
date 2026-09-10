// Flow fields. See cy/navigation/flow_field.h for the argument, and in particular for what the
// affected set of an incremental regeneration is and why it is correct.

#include <cy/navigation/flow_field.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace cy::navigation {
namespace {

constexpr f32 kUnreachable = math::kInfinity;

/// Is (x, z) outside the field? One function rather than six inline comparisons, because the test
/// mixes a signed coordinate with an unsigned extent and writing that six times is six chances to
/// get the conversion wrong. `std::cmp_greater_equal` is the conversion-safe form; a
/// `static_cast<u32>` on the coordinate would compare the same bits and read as if the negative
/// case had already been handled somewhere else.
[[nodiscard]] bool outside(i32 x, i32 z, u32 width, u32 depth) noexcept {
    return x < 0 || z < 0 || std::cmp_greater_equal(x, width) || std::cmp_greater_equal(z, depth);
}

/// The diagonal step's length, so a field over a uniform region produces straight lines rather than
/// staircases.
constexpr f32 kDiagonal = std::numbers::sqrt2_v<f32>;

struct Step {
    i32 dx;
    i32 dz;
    f32 length;
};

constexpr Step kSteps[8] = {{-1, 0, 1.0f},      {1, 0, 1.0f},        {0, -1, 1.0f},
                            {0, 1, 1.0f},       {-1, -1, kDiagonal}, {1, -1, kDiagonal},
                            {-1, 1, kDiagonal}, {1, 1, kDiagonal}};

/// Worst first, so the max-heap is a min-heap on the integrated cost. The tie-break is the cell
/// index, which is what makes the build deterministic — `navigation` requires that outright.
struct WorseFirst {
    const Array<f32>* integration;

    [[nodiscard]] bool operator()(u32 a, u32 b) const noexcept {
        const f32 left = (*integration)[a];
        const f32 right = (*integration)[b];
        if (left != right) {
            return left > right;
        }
        return a > b;
    }
};

}  // namespace

FlowField::FlowField(Allocator& allocator, const FlowFieldParams& params) noexcept
    : params_(params),
      cell_cost_(allocator),
      integration_(allocator),
      direction_(allocator),
      destinations_(allocator) {
    if (params_.cell_size <= 0.0f) {
        params_.cell_size = 1.0f;
    }
    const Vec3 size = params_.region.size();
    width_ = static_cast<u32>(std::max(1.0f, std::ceil(size.x / params_.cell_size)));
    depth_ = static_cast<u32>(std::max(1.0f, std::ceil(size.z / params_.cell_size)));
}

Status FlowField::allocate() noexcept {
    const usize count = static_cast<usize>(width_) * depth_;
    if (Status sized = cell_cost_.resize(count); !sized) {
        return sized;
    }
    if (Status sized = integration_.resize(count); !sized) {
        return sized;
    }
    return direction_.resize(count);
}

i32 FlowField::cell_x(f32 x) const noexcept {
    return static_cast<i32>(std::floor((x - params_.region.min.x) / params_.cell_size));
}

i32 FlowField::cell_z(f32 z) const noexcept {
    return static_cast<i32>(std::floor((z - params_.region.min.z) / params_.cell_size));
}

Vec3 FlowField::cell_centre(u32 index) const noexcept {
    const u32 x = index % width_;
    const u32 z = index / width_;
    return Vec3{params_.region.min.x + ((static_cast<f32>(x) + 0.5f) * params_.cell_size),
                (params_.region.min.y + params_.region.max.y) * 0.5f,
                params_.region.min.z + ((static_cast<f32>(z) + 0.5f) * params_.cell_size)};
}

void FlowField::sample_costs(const NavMesh& mesh, u32 from_x, u32 from_z, u32 to_x,
                             u32 to_z) noexcept {
    const Vec3 extents{params_.cell_size * 0.5f, params_.sample_height, params_.cell_size * 0.5f};
    for (u32 z = from_z; z < to_z; ++z) {
        for (u32 x = from_x; x < to_x; ++x) {
            const u32 index = (z * width_) + x;
            Vec3 nearest;
            const PolyRef ref =
                mesh.find_nearest(cell_centre(index), extents, params_.areas, nearest);
            const NavPoly* poly = mesh.poly(ref);
            if (poly == nullptr) {
                cell_cost_[index] = kUnreachable;
                continue;
            }
            const AreaType area = mesh.effective_area(ref);
            if ((area_bit(area) & params_.areas) == 0) {
                cell_cost_[index] = kUnreachable;
                continue;
            }
            cell_cost_[index] = params_.costs.of(area) * poly->cost;
        }
    }
}

Expected<FlowFieldUpdate, Error> FlowField::integrate(Array<u32>& seeds, bool full) noexcept {
    FlowFieldUpdate update;
    update.cells = cell_count();
    update.full_rebuild = full;

    const WorseFirst worse{&integration_};
    std::ranges::make_heap(seeds.span(), worse);

    while (!seeds.empty()) {
        std::ranges::pop_heap(seeds.span(), worse);
        const u32 current = seeds[seeds.size() - 1];
        seeds.pop_back();
        ++update.cells_visited;

        const f32 here = integration_[current];
        if (here >= kUnreachable) {
            continue;
        }
        const i32 x = static_cast<i32>(current % width_);
        const i32 z = static_cast<i32>(current / width_);
        for (const Step& step : kSteps) {
            const i32 nx = x + step.dx;
            const i32 nz = z + step.dz;
            if (outside(nx, nz, width_, depth_)) {
                continue;
            }
            const u32 neighbour = (static_cast<u32>(nz) * width_) + static_cast<u32>(nx);
            const f32 cost = cell_cost_[neighbour];
            if (cost >= kUnreachable) {
                continue;
            }
            const f32 candidate = here + (step.length * params_.cell_size * cost);
            if (candidate + 1e-4f >= integration_[neighbour]) {
                continue;
            }
            integration_[neighbour] = candidate;
            if (Status pushed = seeds.push_back(neighbour); !pushed) {
                return make_unexpected(pushed.error());
            }
            std::ranges::push_heap(seeds.span(), worse);
        }
    }

    for (u32 index = 0; index < cell_count(); ++index) {
        if (integration_[index] >= kUnreachable) {
            ++update.unreachable;
        }
    }
    return update;
}

void FlowField::build_directions(u32 from_x, u32 from_z, u32 to_x, u32 to_z) noexcept {
    for (u32 z = from_z; z < to_z; ++z) {
        for (u32 x = from_x; x < to_x; ++x) {
            const u32 index = (z * width_) + x;
            direction_[index] = Vec2{};
            if (integration_[index] >= kUnreachable) {
                continue;
            }
            f32 best = integration_[index];
            i32 best_dx = 0;
            i32 best_dz = 0;
            for (const Step& step : kSteps) {
                const i32 nx = static_cast<i32>(x) + step.dx;
                const i32 nz = static_cast<i32>(z) + step.dz;
                if (outside(nx, nz, width_, depth_)) {
                    continue;
                }
                const u32 neighbour = (static_cast<u32>(nz) * width_) + static_cast<u32>(nx);
                if (integration_[neighbour] < best) {
                    best = integration_[neighbour];
                    best_dx = step.dx;
                    best_dz = step.dz;
                }
            }
            if (best_dx == 0 && best_dz == 0) {
                continue;
            }
            const f32 magnitude =
                std::sqrt(static_cast<f32>((best_dx * best_dx) + (best_dz * best_dz)));
            direction_[index] =
                Vec2{static_cast<f32>(best_dx) / magnitude, static_cast<f32>(best_dz) / magnitude};
        }
    }
}

Expected<FlowFieldUpdate, Error> FlowField::build(const NavMesh& mesh,
                                                  Span<const Vec3> destinations) noexcept {
    if (Status allocated = allocate(); !allocated) {
        return make_unexpected(allocated.error());
    }
    destinations_.clear();
    if (Status kept = destinations_.append(destinations); !kept) {
        return make_unexpected(kept.error());
    }
    mesh_version_ = mesh.version();

    sample_costs(mesh, 0, 0, width_, depth_);
    for (f32& value : integration_.span()) {
        value = kUnreachable;
    }

    Array<u32> seeds(integration_.allocator());
    for (const Vec3& destination : destinations) {
        const i32 x = cell_x(destination.x);
        const i32 z = cell_z(destination.z);
        if (outside(x, z, width_, depth_)) {
            continue;
        }
        const u32 index = (static_cast<u32>(z) * width_) + static_cast<u32>(x);
        if (cell_cost_[index] >= kUnreachable) {
            continue;
        }
        integration_[index] = 0.0f;
        if (Status pushed = seeds.push_back(index); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    const Expected<FlowFieldUpdate, Error> update = integrate(seeds, true);
    if (!update) {
        return update;
    }
    build_directions(0, 0, width_, depth_);
    return update;
}

Expected<FlowFieldUpdate, Error> FlowField::regenerate(const NavMesh& mesh,
                                                       const Aabb& dirty) noexcept {
    if (integration_.empty()) {
        return build(mesh, destinations_.span());
    }
    mesh_version_ = mesh.version();

    const i32 lo_x = std::max(0, cell_x(dirty.min.x) - 1);
    const i32 lo_z = std::max(0, cell_z(dirty.min.z) - 1);
    const i32 hi_x = std::min(static_cast<i32>(width_), cell_x(dirty.max.x) + 2);
    const i32 hi_z = std::min(static_cast<i32>(depth_), cell_z(dirty.max.z) + 2);
    if (lo_x >= hi_x || lo_z >= hi_z) {
        FlowFieldUpdate update;
        update.cells = cell_count();
        return update;
    }

    sample_costs(mesh, static_cast<u32>(lo_x), static_cast<u32>(lo_z), static_cast<u32>(hi_x),
                 static_cast<u32>(hi_z));

    // The threshold: no cell below the dirty region's own smallest integration can have RISEN, so
    // nothing below it is reset. See the header.
    f32 threshold = kUnreachable;
    for (i32 z = lo_z; z < hi_z; ++z) {
        for (i32 x = lo_x; x < hi_x; ++x) {
            const u32 index = (static_cast<u32>(z) * width_) + static_cast<u32>(x);
            threshold = std::min(threshold, integration_[index]);
        }
    }

    Array<bool> reset(integration_.allocator());
    if (Status sized = reset.resize(cell_count()); !sized) {
        return make_unexpected(sized.error());
    }
    FlowFieldUpdate update;
    update.cells = cell_count();
    for (u32 index = 0; index < cell_count(); ++index) {
        const bool affected = integration_[index] >= threshold;
        reset[index] = affected;
        if (affected) {
            integration_[index] = kUnreachable;
            ++update.cells_reset;
        }
    }

    Array<u32> seeds(integration_.allocator());
    // Destinations inside the reset set come back at zero; everything else is seeded from the
    // frontier, which is where a value that is still correct meets one that was cleared.
    for (const Vec3& destination : destinations_.span()) {
        const i32 x = cell_x(destination.x);
        const i32 z = cell_z(destination.z);
        if (outside(x, z, width_, depth_)) {
            continue;
        }
        const u32 index = (static_cast<u32>(z) * width_) + static_cast<u32>(x);
        if (cell_cost_[index] >= kUnreachable || !reset[index]) {
            continue;
        }
        integration_[index] = 0.0f;
        if (Status pushed = seeds.push_back(index); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    for (u32 index = 0; index < cell_count(); ++index) {
        if (reset[index] || integration_[index] >= kUnreachable) {
            continue;
        }
        const i32 x = static_cast<i32>(index % width_);
        const i32 z = static_cast<i32>(index / width_);
        bool frontier = false;
        for (const Step& step : kSteps) {
            const i32 nx = x + step.dx;
            const i32 nz = z + step.dz;
            if (outside(nx, nz, width_, depth_)) {
                continue;
            }
            if (reset[(static_cast<u32>(nz) * width_) + static_cast<u32>(nx)]) {
                frontier = true;
                break;
            }
        }
        if (frontier) {
            if (Status pushed = seeds.push_back(index); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }

    Expected<FlowFieldUpdate, Error> integrated = integrate(seeds, false);
    if (!integrated) {
        return integrated;
    }
    update.cells_visited = integrated->cells_visited;
    update.unreachable = integrated->unreachable;
    build_directions(0, 0, width_, depth_);
    return update;
}

Vec3 FlowField::direction_at(Vec3 position) const noexcept {
    const i32 x = cell_x(position.x);
    const i32 z = cell_z(position.z);
    if (outside(x, z, width_, depth_)) {
        return Vec3{};
    }
    const Vec2 direction = direction_[(static_cast<u32>(z) * width_) + static_cast<u32>(x)];
    return Vec3{direction.x, 0.0f, direction.y};
}

f32 FlowField::cost_at(Vec3 position) const noexcept {
    const i32 x = cell_x(position.x);
    const i32 z = cell_z(position.z);
    if (outside(x, z, width_, depth_)) {
        return kUnreachable;
    }
    return integration_[(static_cast<u32>(z) * width_) + static_cast<u32>(x)];
}

bool FlowField::reachable_at(Vec3 position) const noexcept {
    return cost_at(position) < kUnreachable;
}

// --- The cache -----------------------------------------------------------------------------------

FlowFieldCache::FlowFieldCache(Allocator& allocator, const FlowFieldParams& params) noexcept
    : allocator_(&allocator), params_(params), fields_(allocator) {}

FlowFieldCache::~FlowFieldCache() {
    for (Entry* entry : fields_.span()) {
        destroy(entry);
    }
}

void FlowFieldCache::destroy(Entry* entry) noexcept {
    if (entry == nullptr) {
        return;
    }
    entry->~Entry();
    allocator_->deallocate(entry, sizeof(Entry), alignof(Entry));
}

Expected<FlowField*, Error> FlowFieldCache::acquire(const NavMesh& mesh,
                                                    Vec3 destination) noexcept {
    for (Entry* entry : fields_.span()) {
        if (distance_squared(entry->destination, destination) <=
            (params_.cell_size * params_.cell_size * 0.25f)) {
            entry->field.acquire();
            return &entry->field;
        }
    }
    void* storage = allocator_->allocate(sizeof(Entry), alignof(Entry));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no room for a flow field");
    }
    auto* entry = construct_at<Entry>(storage, *allocator_, params_, destination);
    const Vec3 targets[1] = {destination};
    if (Expected<FlowFieldUpdate, Error> built =
            entry->field.build(mesh, Span<const Vec3>(targets));
        !built) {
        destroy(entry);
        return make_unexpected(built.error());
    }
    if (Status pushed = fields_.push_back(entry); !pushed) {
        destroy(entry);
        return make_unexpected(pushed.error());
    }
    entry->field.acquire();
    return &entry->field;
}

void FlowFieldCache::release(FlowField* field) noexcept {
    if (field == nullptr) {
        return;
    }
    // Only a field this cache owns. Releasing a stranger's would decrement a count this cache does
    // not keep, and the mismatch would surface later as a field collected while it was still in
    // use — the hardest kind of lifetime bug to trace back to its cause.
    for (const Entry* entry : fields_.span()) {
        if (&entry->field == field) {
            field->release();
            return;
        }
    }
}

u32 FlowFieldCache::collect() noexcept {
    u32 removed = 0;
    usize index = 0;
    while (index < fields_.size()) {
        if (fields_[index]->field.users() == 0) {
            destroy(fields_[index]);
            fields_.remove_unordered(index);
            ++removed;
            continue;
        }
        ++index;
    }
    return removed;
}

u32 FlowFieldCache::regenerate(const NavMesh& mesh, const Aabb& dirty) noexcept {
    u32 updated = 0;
    for (Entry* entry : fields_.span()) {
        if (entry->field.regenerate(mesh, dirty)) {
            ++updated;
        }
    }
    return updated;
}

}  // namespace cy::navigation
