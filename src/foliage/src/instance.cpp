// The compact instance, derived identity, cluster building and the hierarchical cull. See
// instance.h for the two design notes: where the sixteen bytes went, and why identity is derived.

#include <cy/foliage/instance.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::foliage {

namespace {

/// The seed every identity hash in CyberFoliage descends from. Fixed, never `hash_seed()`, for the
/// reason `world::kWorldHashSeed` and `terrain::kTerrainHashSeed` are fixed: an identifier that
/// changes between two runs of one cooker is not an identifier.
constexpr u64 kFoliageHashSeed = 0x6379'6265'7266'6c67ULL;  // "cybe" "rflg"

[[nodiscard]] u16 quantise_unit(f64 value) noexcept {
    const f64 clamped = math::clamp(value, 0.0, 1.0);
    // `lround` rather than `+ 0.5`: the cast truncates toward zero, so the addition is only a
    // rounding for non-negative values and is a habit that becomes a bug the first time somebody
    // quantises a signed quantity here.
    return static_cast<u16>(std::lround(clamped * 65535.0));
}

[[nodiscard]] f64 dequantise_unit(u16 value) noexcept {
    return static_cast<f64>(value) * (1.0 / 65535.0);
}

/// A tilt in radians onto the signed byte, over the +-32 degrees the record carries. The clamp is
/// where the storage limit becomes visible; `PlacementRule::max_tilt_degrees` is where a caller is
/// told about it.
constexpr f32 kMaxTiltRadians = 0.5585F;  // 32 degrees

[[nodiscard]] i8 quantise_tilt(f32 radians) noexcept {
    const f32 clamped = math::clamp(radians, -kMaxTiltRadians, kMaxTiltRadians);
    return static_cast<i8>(math::clamp(clamped / kMaxTiltRadians * 127.0F, -127.0F, 127.0F));
}

}  // namespace

ClusterId cluster_identity(u64 seed, ClusterCoord coord, u32 graph_version) noexcept {
    // The rule graph version participates, so bumping it renames every cluster — see the field's
    // own comment in `PlacementRuleSet`. Fixed seed throughout.
    // `hash_integer(value, seed)` — the WORLD seed is the value being mixed and this module's own
    // constant is the seed, which is the pairing every identity hash in the engine uses. Named
    // locals rather than the call's own argument order, because `readability-suspicious-call-
    // argument` reads the two as swapped and it is right to ask.
    const u64 mixed_value = seed;
    const u64 mixer_seed = kFoliageHashSeed;
    u64 mixed = hash_integer(mixed_value, mixer_seed);
    mixed = hash_combine(mixed, static_cast<u64>(static_cast<u32>(coord.x)));
    mixed = hash_combine(mixed, static_cast<u64>(static_cast<u32>(coord.z)));
    mixed = hash_combine(mixed, coord.level);
    mixed = hash_combine(mixed, graph_version);
    // Never zero: a zero identity is the invalid one, and a cluster that hashed to it would be
    // indistinguishable from an absent one for the rest of its life.
    return ClusterId{mixed == 0 ? 1ULL : mixed};
}

InstanceId instance_identity(u64 seed, ClusterId cluster, u32 slot) noexcept {
    // SUBSTREAM THEN DRAW. See instance.h: a `fold_multiply(cluster, slot)` is `cluster * slot` for
    // small operands, and a product is not injective — the M10 spike found exactly that defect in
    // its own first derivation and reported 93 of 455 overrides mis-bound under a scheme that
    // cannot mis-bind.
    const determinism::StreamId stream =
        determinism::substream(determinism::stream_id(kIdentityStream), cluster.value);
    const determinism::RandomStream draws(seed, stream, determinism::StreamPurpose::Authoritative);
    const u64 value = draws.draw(generation_point(), slot, 0);
    return InstanceId{value == 0 ? 1ULL : value};
}

void ClusterBounds::encode(const world::WorldVec3d& position,
                           FoliageInstance& instance) const noexcept {
    const f64 sx = span_x();
    const f64 sz = span_z();
    const f32 sy = span_y();
    instance.qx = quantise_unit(sx > 0.0 ? (position.x - min_x) / sx : 0.0);
    instance.qz = quantise_unit(sz > 0.0 ? (position.z - min_z) / sz : 0.0);
    instance.qy = quantise_unit(
        sy > 0.0F ? (position.y - static_cast<f64>(min_y)) / static_cast<f64>(sy) : 0.0);
}

world::WorldVec3d ClusterBounds::decode(const FoliageInstance& instance) const noexcept {
    return world::WorldVec3d{
        min_x + (dequantise_unit(instance.qx) * span_x()),
        static_cast<f64>(min_y) + (dequantise_unit(instance.qy) * static_cast<f64>(span_y())),
        min_z + (dequantise_unit(instance.qz) * span_z())};
}

f32 ClusterBounds::radius() const noexcept {
    const f64 half_x = span_x() * 0.5;
    const f64 half_z = span_z() * 0.5;
    const f64 half_y = static_cast<f64>(span_y()) * 0.5;
    return static_cast<f32>(std::sqrt((half_x * half_x) + (half_y * half_y) + (half_z * half_z)));
}

// --- FoliageCluster
// ------------------------------------------------------------------------------

FoliageCluster::FoliageCluster(Allocator& allocator, ClusterId id, ClusterCoord coord,
                               const ClusterBounds& bounds) noexcept
    : id_(id),
      coord_(coord),
      bounds_(bounds),
      instances_(allocator),
      blocks_(allocator),
      species_(allocator) {}

SpeciesId FoliageCluster::species_at(u8 slot) const noexcept {
    return slot < species_.size() ? species_[slot] : kInvalidSpecies;
}

const FoliageInstance* FoliageCluster::at(u32 slot) const noexcept {
    return slot < instances_.size() ? &instances_[slot] : nullptr;
}

InstanceId FoliageCluster::identity_at(u64 seed, u32 slot) const noexcept {
    if (slot >= instances_.size()) {
        return InstanceId{};
    }
    return instance_identity(seed, id_, slot);
}

u32 FoliageCluster::slot_of(u64 seed, InstanceId identity) const noexcept {
    for (usize index = 0; index < instances_.size(); ++index) {
        if (instance_identity(seed, id_, static_cast<u32>(index)) == identity) {
            return static_cast<u32>(index);
        }
    }
    return kNoSlot;
}

Status FoliageCluster::set_flags(u32 slot, InstanceFlags flags) noexcept {
    if (slot >= instances_.size()) {
        return fail(ErrorCode::OutOfRange, "no such instance slot in this cluster");
    }
    FoliageInstance& instance = instances_[slot];
    const bool was_drawable = instance.flags.drawable();
    const bool now_drawable = flags.drawable();
    instance.flags = flags;
    if (was_drawable == now_drawable) {
        return ok();
    }
    // The owning block's drawable count is maintained rather than recomputed, so a diagnostic and a
    // draw cannot disagree about how many instances a species is contributing this frame.
    for (SpeciesBlock& block : blocks_) {
        if (slot >= block.first && slot < block.first + block.count) {
            if (now_drawable) {
                ++block.drawable;
            } else if (block.drawable > 0) {
                --block.drawable;
            }
            break;
        }
    }
    return ok();
}

Status FoliageCluster::replace_instance(u32 slot, const FoliageInstance& instance) noexcept {
    if (slot >= instances_.size()) {
        return fail(ErrorCode::OutOfRange, "no such instance slot in this cluster");
    }
    if (instances_[slot].species_slot != instance.species_slot) {
        return fail(ErrorCode::InvalidArgument,
                    "replacing an instance may not change its species: the blocks are contiguous "
                    "runs and every slot after it would be renumbered");
    }
    // The record is written in two steps so the block's drawable count is maintained in exactly one
    // place: the quantised half here, and the flags through `set_flags()`, which is the function
    // that owns the count. Writing both at once would need the delta computed a second time.
    const InstanceFlags flags = instance.flags;
    const InstanceFlags previous = instances_[slot].flags;
    instances_[slot] = instance;
    instances_[slot].flags = previous;
    return set_flags(slot, flags);
}

u64 FoliageCluster::bytes() const noexcept {
    return (static_cast<u64>(instances_.size()) * sizeof(FoliageInstance)) +
           (static_cast<u64>(blocks_.size()) * sizeof(SpeciesBlock)) +
           (static_cast<u64>(species_.size()) * sizeof(SpeciesId));
}

// --- ClusterBuilder
// ------------------------------------------------------------------------------

ClusterBuilder::ClusterBuilder(Allocator& allocator, const ClusterPolicy& policy, ClusterId id,
                               ClusterCoord coord, const ClusterBounds& bounds) noexcept
    : allocator_(&allocator),
      policy_(policy),
      id_(id),
      coord_(coord),
      bounds_(bounds),
      staged_(allocator),
      species_(allocator) {
    report_.cluster = id;
}

Expected<u8, Error> ClusterBuilder::slot_for(SpeciesId species) noexcept {
    for (usize index = 0; index < species_.size(); ++index) {
        if (species_[index] == species) {
            return static_cast<u8>(index);
        }
    }
    if (species_.size() >= kMaxClusterSpecies) {
        return fail(ErrorCode::OutOfRange, "cluster already carries the maximum species count");
    }
    if (Status pushed = species_.push_back(species); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u8>(species_.size() - 1);
}

Status ClusterBuilder::add(SpeciesId species, const world::WorldVec3d& position, f32 yaw_radians,
                           f32 scale_unit, u8 variation, u8 age, f32 tilt_x_radians,
                           f32 tilt_z_radians, InstanceFlags flags) noexcept {
    if (staged_.size() >= policy_.max_instances) {
        // Reported rather than silently truncated: a world whose clusters hit the ceiling has a
        // policy that is wrong for it, and that is a thing a cooker should print.
        ++report_.dropped_over_ceiling;
        return ok();
    }
    Expected<u8, Error> slot = slot_for(species);
    if (!slot) {
        ++report_.dropped_species_overflow;
        return ok();
    }
    Staged staged;
    staged.species = species;
    bounds_.encode(position, staged.instance);
    // The yaw is wrapped into [0, 2pi) before quantising rather than clamped: a rule producing
    // -0.1 rad and one producing 2pi-0.1 mean the same rotation, and a clamp would flatten one of
    // them onto zero.
    const f32 turns = yaw_radians * (1.0F / math::kTwoPi);
    const f32 wrapped = turns - std::floor(turns);
    staged.instance.yaw = static_cast<u16>(math::clamp(wrapped, 0.0F, 0.99998F) * 65536.0F);
    staged.instance.scale = quantise_unit(static_cast<f64>(math::clamp(scale_unit, 0.0F, 1.0F)));
    staged.instance.variation = variation;
    staged.instance.age = age;
    staged.instance.tilt_x = quantise_tilt(tilt_x_radians);
    staged.instance.tilt_z = quantise_tilt(tilt_z_radians);
    staged.instance.species_slot = slot.value();
    staged.instance.flags = flags;
    return staged_.push_back(staged);
}

namespace {

/// The canonical order. **A total order over the STORED record**, which is what makes a slot stable
/// whatever order placement accepted candidates in — see `ClusterBuilder::finish()`'s comment.
[[nodiscard]] bool staged_before(const FoliageInstance& a, const FoliageInstance& b) noexcept {
    if (a.species_slot != b.species_slot) {
        return a.species_slot < b.species_slot;
    }
    if (a.qz != b.qz) {
        return a.qz < b.qz;
    }
    if (a.qx != b.qx) {
        return a.qx < b.qx;
    }
    if (a.qy != b.qy) {
        return a.qy < b.qy;
    }
    if (a.yaw != b.yaw) {
        return a.yaw < b.yaw;
    }
    if (a.scale != b.scale) {
        return a.scale < b.scale;
    }
    if (a.variation != b.variation) {
        return a.variation < b.variation;
    }
    if (a.age != b.age) {
        return a.age < b.age;
    }
    return a.flags.bits < b.flags.bits;
}

/// Insertion sort. The arrays are a few thousand entries and the comparison is cheap; the reason it
/// is written here rather than reached for is that the ORDER is the property this module is judged
/// on, and a sort whose tie-breaking depended on a library's stability guarantee would be a
/// property nobody can read in this file.
void sort_staged(Span<FoliageInstance> instances, Span<SpeciesId> species) noexcept {
    for (usize index = 1; index < instances.size(); ++index) {
        const FoliageInstance key = instances[index];
        const SpeciesId key_species = species[index];
        usize hole = index;
        while (hole > 0 && staged_before(key, instances[hole - 1])) {
            instances[hole] = instances[hole - 1];
            species[hole] = species[hole - 1];
            --hole;
        }
        instances[hole] = key;
        species[hole] = key_species;
    }
}

}  // namespace

Expected<FoliageCluster, Error> ClusterBuilder::finish() noexcept {
    Array<FoliageInstance> instances(*allocator_);
    Array<SpeciesId> owners(*allocator_);
    if (Status reserved = instances.reserve(staged_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    if (Status reserved = owners.reserve(staged_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (const Staged& staged : staged_) {
        if (Status pushed = instances.push_back(staged.instance); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = owners.push_back(staged.species); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    sort_staged(instances.span(), owners.span());

    FoliageCluster cluster(*allocator_, id_, coord_, bounds_);
    cluster.instances_ = static_cast<Array<FoliageInstance>&&>(instances);
    for (SpeciesId species : species_) {
        if (Status pushed = cluster.species_.push_back(species); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    // Blocks are derived from the sorted array rather than tracked while staging: the sort is what
    // makes a species' instances contiguous, so deriving them afterwards is the only way they can
    // be right.
    SpeciesBlock current;
    bool open = false;
    for (usize index = 0; index < cluster.instances_.size(); ++index) {
        const FoliageInstance& instance = cluster.instances_[index];
        const SpeciesId species = cluster.species_at(instance.species_slot);
        if (!open || !(current.species == species)) {
            if (open) {
                if (Status pushed = cluster.blocks_.push_back(current); !pushed) {
                    return make_unexpected(pushed.error());
                }
            }
            current = SpeciesBlock{};
            current.species = species;
            current.first = static_cast<u32>(index);
            current.count = 0;
            current.drawable = 0;
            open = true;
        }
        ++current.count;
        current.drawable += instance.flags.drawable() ? 1U : 0U;
    }
    if (open) {
        if (Status pushed = cluster.blocks_.push_back(current); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    report_.instances = static_cast<u32>(cluster.instances_.size());
    report_.species = static_cast<u32>(cluster.species_.size());
    report_.bytes = cluster.bytes();
    return cluster;
}

// --- ClusterStore
// --------------------------------------------------------------------------------

ClusterStore::ClusterStore(Allocator& allocator) noexcept
    : clusters_(allocator), index_(allocator) {}

Status ClusterStore::insert(FoliageCluster cluster) noexcept {
    const ClusterId id = cluster.id();
    if (index_.contains(id.value)) {
        return fail(ErrorCode::AlreadyExists, "a cluster with this identity is already resident");
    }
    if (Status pushed = clusters_.push_back(static_cast<FoliageCluster&&>(cluster)); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> placed = index_.insert(id.value, clusters_.size() - 1); !placed) {
        clusters_.pop_back();
        return fail(placed.error().code, placed.error().message);
    }
    return ok();
}

bool ClusterStore::evict(ClusterId cluster) noexcept {
    const usize* slot = index_.find(cluster.value);
    if (slot == nullptr) {
        return false;
    }
    const usize index = *slot;
    const usize last = clusters_.size() - 1;
    if (index != last) {
        // Swap-remove, and the moved cluster's index entry is repaired. The order of `clusters_` is
        // therefore not the insertion order — nothing depends on it, and the two places that need
        // an order (a diagnostic and a save) sort by identity.
        clusters_[index] = static_cast<FoliageCluster&&>(clusters_[last]);
        if (usize* moved = index_.find(clusters_[index].id().value); moved != nullptr) {
            *moved = index;
        }
    }
    clusters_.pop_back();
    return index_.remove(cluster.value);
}

FoliageCluster* ClusterStore::find(ClusterId cluster) noexcept {
    usize* slot = index_.find(cluster.value);
    return slot == nullptr ? nullptr : &clusters_[*slot];
}

const FoliageCluster* ClusterStore::find(ClusterId cluster) const noexcept {
    const usize* slot = index_.find(cluster.value);
    return slot == nullptr ? nullptr : &clusters_[*slot];
}

u64 ClusterStore::instance_count() const noexcept {
    u64 total = 0;
    for (const FoliageCluster& cluster : clusters_) {
        total += cluster.size();
    }
    return total;
}

u64 ClusterStore::bytes() const noexcept {
    u64 total = 0;
    for (const FoliageCluster& cluster : clusters_) {
        total += cluster.bytes();
    }
    return total;
}

namespace {

/// A sphere against the view's planes. Conservative: a cluster straddling a plane is kept, which is
/// what a bulk rejection must be — rejecting a straddling cluster would drop instances that are on
/// screen.
[[nodiscard]] bool sphere_visible(const CullView& view, const world::WorldVec3d& centre,
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

[[nodiscard]] f64 distance_between(const world::WorldVec3d& a,
                                   const world::WorldVec3d& b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

/// The tier a cluster draws at: the coarsest tier any of its species agrees to at this projected
/// size. Per cluster and not per instance, because the cluster is the drawing unit — an instance
/// tier would need a sort every frame, which is the per-instance CPU work the module is built to
/// avoid.
[[nodiscard]] DetailTier cluster_tier(const SpeciesLibrary& library, const FoliageCluster& cluster,
                                      f32 pixels) noexcept {
    DetailTier coarsest = DetailTier::Detailed;
    bool any = false;
    for (const SpeciesBlock& block : cluster.blocks()) {
        const SpeciesDeclaration* species = library.find(block.species);
        if (species == nullptr) {
            continue;
        }
        const DetailTier tier = tier_for_pixels(*species, pixels);
        if (!any || static_cast<u32>(tier) > static_cast<u32>(coarsest)) {
            coarsest = tier;
            any = true;
        }
    }
    return coarsest;
}

}  // namespace

Expected<CullResult, Error> ClusterStore::cull(const SpeciesLibrary& library, const CullView& view,
                                               Array<VisibleCluster>& out) const noexcept {
    CullResult result;
    for (const FoliageCluster& cluster : clusters_) {
        ++result.clusters_tested;
        const world::WorldVec3d centre = cluster.bounds().centre();
        const f64 radius = static_cast<f64>(cluster.bounds().radius());
        const f64 distance = distance_between(view.eye, centre);
        // The cluster test, and the whole of "most instances are rejected in bulk": an instance
        // inside a rejected cluster is never examined, and the counter below is what makes that a
        // measurement rather than an assurance.
        const bool too_far = distance - radius > static_cast<f64>(view.max_distance_metres);
        if (too_far || !sphere_visible(view, centre, radius)) {
            result.instances_rejected_in_bulk += static_cast<u32>(cluster.size());
            continue;
        }
        ++result.clusters_visible;
        result.instances_visible += static_cast<u32>(cluster.size());
        const f32 pixels = distance > 1e-6
                               ? static_cast<f32>(static_cast<f64>(view.pixels_per_metre) *
                                                  (radius * 2.0) / distance)
                               : view.pixels_per_metre;
        VisibleCluster visible;
        visible.cluster = cluster.id();
        visible.tier = cluster_tier(library, cluster, pixels);
        visible.distance_metres = static_cast<f32>(distance);
        visible.instances = static_cast<u32>(cluster.size());
        if (Status pushed = out.push_back(visible); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return result;
}

}  // namespace cy::foliage
