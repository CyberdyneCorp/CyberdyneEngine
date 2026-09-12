#pragma once
// The compact instance, its identity, and the cluster that holds it. M10 task 2.4.
//
// `foliage` — "Foliage instances are not entities": "An instance SHALL be a small record —
// position, rotation, scale, variation, and flags — SIZED SO THAT MILLIONS CAN EXIST IN MEMORY and
// be culled and drawn entirely on the GPU", and "their per-instance memory SHALL be TENS OF BYTES
// rather than hundreds".
//
// ================================================================================================
// SIXTEEN BYTES, AND WHERE EACH OF THEM WENT
// ================================================================================================
//
// `static_assert(sizeof(FoliageInstance) == 16)` at the bottom of this file is the requirement. The
// arithmetic that gets there:
//
//   * POSITION IS CLUSTER-RELATIVE AND QUANTISED. Three `u16` over the cluster's own bounds. A
//     64 m cluster resolves to 1 mm horizontally, which is finer than any placement rule's own
//     precision; an absolute `WorldVec3d` would have cost 24 bytes on its own and would have made
//     a million trees 24 MB of position before anything else.
//   * ROTATION IS A YAW AND A TILT, NOT A QUATERNION. A plant stands up. `yaw` is a full `u16`
//     turn and `tilt_x`/`tilt_z` are signed bytes over +-32 degrees, which is the range in which a
//     trunk still looks planted. A `Quat` would have been 16 bytes by itself.
//   * IDENTITY IS NOT STORED. It is DERIVED from the cluster and the slot — see
//     `instance_identity()` — which is both the specification's own words ("a persistent identity
//     derived from its cluster and index") and the only shape that keeps storage proportional to
//     nothing at all.
//
// ================================================================================================
// IDENTITY IS DERIVED, AND THE SPIKE SAYS EXACTLY HOW
// ================================================================================================
//
// M10's PCG spike (design.md §1) measured four identity schemes against 7 877 hand-placed overrides
// rebound across twelve regenerations. A traversal COUNTER mis-bound 3 351 of them — 43% — on an
// ordinary FULL regeneration, before partial regeneration was involved at all. A RANK among
// survivors mis-bound 292. A value DERIVED from stable identifiers mis-bound ZERO.
//
// It also recorded the defect the spike found in itself, and this file repeats the fix rather than
// the defect: `fold_multiply(region + 1, slot + 1)` is, for small operands, plain `a * b` — the
// product's high half is zero, so nothing folds — and `a * b` is not injective, so region 1 slot 5
// and region 5 slot 1 were one identity. The correct derivation is SUBSTREAM THEN DRAW, and
// `instance_identity()` is that and nothing else.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/random.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/foliage/species.h>
#include <cy/world/coordinates.h>

namespace cy::foliage {

/// An instance's stable identity. Derived, never stored in the instance and never a counter.
///
/// It is what an exception anchors to, what a promoted entity carries back, and what a save
/// records. `foliage`: "a promoted instance SHALL carry a persistent identity derived from its
/// cluster and index, so that damage, removal, and state SURVIVE THE ROUND TRIP and a felled tree
/// does not return."
struct InstanceId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(InstanceId, InstanceId) noexcept = default;
    friend constexpr bool operator<(InstanceId a, InstanceId b) noexcept {
        return a.value < b.value;
    }
};

/// A cluster's identity: opaque, stable, derived from the world seed, the cluster's grid coordinate
/// and the rule graph version.
///
/// Opaque for the reason `world::CellId` is: the moment a consumer can read x out of it, the
/// cluster grid is no longer free to change shape, and this identifier is exactly what a cooked
/// cluster, a save and a streaming cache key on.
struct ClusterId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ClusterId, ClusterId) noexcept = default;
    friend constexpr bool operator<(ClusterId a, ClusterId b) noexcept { return a.value < b.value; }
};

/// A cluster's place on the generation grid. Level exists because a macro representation covers a
/// coarser square than the clusters it stands for; level 0 is the FINEST, matching
/// `world::CellCoord` and `terrain::TileCoord`.
struct ClusterCoord {
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(const ClusterCoord&, const ClusterCoord&) noexcept = default;
};

/// The stream every derived identity in CyberFoliage descends from. A literal, so the identifier is
/// a compile-time constant and is the same number in every process on every machine forever.
inline constexpr const char* kIdentityStream = "foliage.identity";

/// The stream placement draws its own randomness from. Separate from the identity stream, because
/// "consuming randomness in one does not shift another's sequence" is a property of
/// `simulation-and-determinism`'s streams and the whole reason a placement that added one draw per
/// candidate must not renumber every instance in the world.
inline constexpr const char* kPlacementStream = "foliage.placement";

/// The moment every generation draw is taken at. Generation is not a tick: a region regenerated at
/// tick 900 000 must produce what it produced at tick 0, so the point is a constant and the
/// per-draw variation comes from the substream and the sample index.
[[nodiscard]] inline determinism::SimulationPoint generation_point() noexcept {
    return determinism::SimulationPoint{};
}

/// A cluster's identity under a seed and a rule graph version.
///
/// The version participates so that "changing a rule graph regenerates the region" is not a thing
/// somebody has to remember to do: a cluster generated under version 3 has a different identity
/// from the same square under version 4, so a cache keyed on it reports a miss rather than serving
/// the old forest, and the exceptions anchored to the old identity go through the re-resolution
/// path in exceptions.h instead of silently binding to a different tree.
[[nodiscard]] ClusterId cluster_identity(u64 seed, ClusterCoord coord, u32 graph_version) noexcept;

/// One instance's identity. **Substream then draw** — see the header note for why this is not a
/// multiply.
///
/// `slot` is the instance's own index within its cluster, which is stable because a cluster's
/// instances are written in a canonical order (`ClusterBuilder::finish()` sorts them) rather than
/// in the order a traversal accepted them.
[[nodiscard]] InstanceId instance_identity(u64 seed, ClusterId cluster, u32 slot) noexcept;

/// Per-instance flags. One byte; every bit is a thing some system needs to know without loading
/// anything else.
struct InstanceFlags {
    u8 bits = 0;

    /// This instance has been promoted to an entity and MUST NOT be drawn as a GPU instance.
    /// `foliage` — "the GPU instance SHALL be suppressed so it is not drawn twice".
    static constexpr u8 kPromoted = 1U << 0U;
    /// An exception moved or added this instance; it did not come out of the rules.
    static constexpr u8 kException = 1U << 1U;
    /// Gameplay removed it. Kept in the block so the slot's identity does not shift; not drawn.
    static constexpr u8 kRemoved = 1U << 2U;
    /// Felled: the plant exists but is lying down. Its state lives in the exception, not here.
    static constexpr u8 kFelled = 1U << 3U;
    /// Author-painted rather than generated. Never removed by a rule change.
    static constexpr u8 kPainted = 1U << 4U;

    [[nodiscard]] constexpr bool has(u8 flag) const noexcept { return (bits & flag) != 0; }
    constexpr void set(u8 flag) noexcept { bits = static_cast<u8>(bits | flag); }
    constexpr void clear(u8 flag) noexcept { bits = static_cast<u8>(bits & ~flag); }
    /// Whether this instance contributes a GPU instance at all.
    [[nodiscard]] constexpr bool drawable() const noexcept {
        return !has(kPromoted) && !has(kRemoved);
    }

    friend constexpr bool operator==(InstanceFlags, InstanceFlags) noexcept = default;
};

/// The compact record. Sixteen bytes; see the header note for where each went.
///
/// Positions are quantised against the CLUSTER's bounds, so decoding one needs the cluster — which
/// is exactly the coupling that makes the record small, and is why `ClusterBounds::decode()` rather
/// than a member function is how a position is read.
struct FoliageInstance {
    /// Cluster-relative position, quantised. `ClusterBounds` maps them back to metres.
    u16 qx = 0;
    u16 qy = 0;
    u16 qz = 0;
    /// Yaw about +Y as a full `u16` turn: 0 is +X, 0x4000 is a quarter turn. 0.0055 degrees of
    /// resolution, which no viewer can distinguish from continuous.
    u16 yaw = 0;
    /// Scale, quantised over the species' declared [scale_min, scale_max].
    u16 scale = 0;
    /// Tilt from vertical, +-32 degrees over a signed byte. A plant on a slope leans with it.
    i8 tilt_x = 0;
    i8 tilt_z = 0;
    /// Which authored variation. Indexes the species' own list.
    u8 variation = 0;
    /// Index into the cluster's species table, NOT a `SpeciesId`: a cluster carries at most
    /// `kMaxClusterSpecies` species and a full identity per instance would have cost eight bytes
    /// to say what one says.
    u8 species_slot = 0;
    /// 0..255 over the species' own maturity range. `foliage` — "Rules SHALL produce species
    /// selection, density, scale, orientation, variation, and AGE."
    u8 age = 0;
    InstanceFlags flags;

    friend constexpr bool operator==(const FoliageInstance&,
                                     const FoliageInstance&) noexcept = default;
};

static_assert(sizeof(FoliageInstance) == 16,
              "`foliage` requires per-instance memory in TENS of bytes: a million trees is 16 MB "
              "at this size and 100+ MB the moment a position or a rotation grows");
static_assert(alignof(FoliageInstance) == 2, "arrays of instances stay tightly packed");

/// The largest number of distinct species one cluster may carry. `FoliageInstance::species_slot` is
/// a byte, so the hard ceiling is 256; sixteen is the policy, because a cluster mixing more than
/// that is a cluster whose per-species instance blocks are each too small to draw usefully, and the
/// builder reports the overflow rather than silently dropping a species.
inline constexpr u32 kMaxClusterSpecies = 16;

/// A cluster's extent, and the quantisation that lives on it. See `FoliageInstance`.
///
/// f64 horizontally for the reason every absolute position in this engine is f64: a cluster a
/// thousand kilometres out has 64 mm of f32 spacing, and the whole point of quantising against the
/// cluster is that the result is exact regardless of where the cluster is.
struct ClusterBounds {
    f64 min_x = 0.0;
    f64 min_z = 0.0;
    f64 max_x = 0.0;
    f64 max_z = 0.0;
    f32 min_y = 0.0F;
    f32 max_y = 0.0F;

    [[nodiscard]] f64 span_x() const noexcept { return max_x - min_x; }
    [[nodiscard]] f64 span_z() const noexcept { return max_z - min_z; }
    [[nodiscard]] f32 span_y() const noexcept { return max_y - min_y; }

    /// Quantise an absolute position into the instance's three `u16`. Positions outside the bounds
    /// are clamped: a cluster's own extent is what an instance is addressed against, and an
    /// instance outside it is a builder defect the builder reports rather than a position this
    /// function invents an encoding for.
    void encode(const world::WorldVec3d& position, FoliageInstance& instance) const noexcept;
    /// The absolute position of an instance.
    [[nodiscard]] world::WorldVec3d decode(const FoliageInstance& instance) const noexcept;

    [[nodiscard]] bool contains(f64 x, f64 z) const noexcept {
        return x >= min_x && x < max_x && z >= min_z && z < max_z;
    }
    [[nodiscard]] world::WorldVec3d centre() const noexcept {
        return world::WorldVec3d{(min_x + max_x) * 0.5, static_cast<f64>((min_y + max_y) * 0.5F),
                                 (min_z + max_z) * 0.5};
    }
    /// Radius of the sphere containing the cluster. What hierarchical culling tests first.
    [[nodiscard]] f32 radius() const noexcept;
};

/// One species' contiguous run of instances inside a cluster. `foliage` — clusters carry
/// "per-species instance blocks".
///
/// Contiguous and not interleaved because drawing is per species: one block is one draw's instance
/// range, and an interleaved layout would need a gather before every draw or a sort every frame.
struct SpeciesBlock {
    SpeciesId species;
    u32 first = 0;
    u32 count = 0;
    /// How many of the block's instances are currently drawable — the rest are promoted or removed.
    /// Maintained by the cluster rather than recomputed, so a diagnostic and a draw agree.
    u32 drawable = 0;
};

/// The per-cluster wind metadata `foliage` names. It is per CLUSTER and not per instance because
/// "Wind response SHALL be evaluated on the GPU as part of geometry processing, and SHALL NOT
/// REQUIRE PER-INSTANCE CPU WORK" — one of these is what the CPU prepares each frame, and a test in
/// test_wind.cpp counts the preparations against the cluster count rather than the instance count.
struct ClusterWind {
    /// The wind vector sampled at the cluster's centre, in metres per second.
    Vec3 wind{0.0F, 0.0F, 0.0F};
    /// A phase offset derived from the cluster's identity, so two adjacent clusters do not sway in
    /// lockstep. Derived and not random, so two machines agree.
    f32 phase = 0.0F;
    /// The finest wind detail this cluster is allowed this frame. Distance and budget, resolved.
    WindDetail detail = WindDetail::Sway;
    /// The field version the sample came from, so a consumer holding a derived value detects
    /// staleness by comparing a number rather than by re-sampling.
    u64 field_version = 0;
};

/// One cluster: bounds, per-species blocks, detail metadata, wind metadata, streaming information.
/// `foliage` — "Foliage clusters", in that requirement's own list.
class FoliageCluster {
public:
    FoliageCluster(Allocator& allocator, ClusterId id, ClusterCoord coord,
                   const ClusterBounds& bounds) noexcept;

    FoliageCluster(const FoliageCluster&) = delete;
    FoliageCluster& operator=(const FoliageCluster&) = delete;
    FoliageCluster(FoliageCluster&&) noexcept = default;
    FoliageCluster& operator=(FoliageCluster&&) noexcept = default;
    ~FoliageCluster() = default;

    [[nodiscard]] ClusterId id() const noexcept { return id_; }
    [[nodiscard]] ClusterCoord coord() const noexcept { return coord_; }
    [[nodiscard]] const ClusterBounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] Span<const FoliageInstance> instances() const noexcept {
        return instances_.span();
    }
    [[nodiscard]] Span<const SpeciesBlock> blocks() const noexcept { return blocks_.span(); }
    [[nodiscard]] usize size() const noexcept { return instances_.size(); }

    /// The species a slot names. `kInvalidSpecies` for a slot the cluster does not carry.
    [[nodiscard]] SpeciesId species_at(u8 slot) const noexcept;
    /// The instance at `slot`, or null.
    [[nodiscard]] const FoliageInstance* at(u32 slot) const noexcept;
    /// The identity of the instance at `slot` under `seed`. Derived; nothing is stored.
    [[nodiscard]] InstanceId identity_at(u64 seed, u32 slot) const noexcept;
    /// The slot an identity names, or `kNoSlot`. Linear over the cluster: a cluster holds a few
    /// thousand instances and an index keyed on a derived identity would cost eight bytes an
    /// instance to accelerate a call that happens when a tree is felled.
    [[nodiscard]] u32 slot_of(u64 seed, InstanceId identity) const noexcept;

    static constexpr u32 kNoSlot = 0xFFFF'FFFFU;

    /// Set the flags on one instance, keeping the owning block's `drawable` count in step.
    [[nodiscard]] Status set_flags(u32 slot, InstanceFlags flags) noexcept;

    /// Replace one instance's record wholesale. The only other mutator, and it exists for exactly
    /// one caller: an exception that MOVED an instance (exceptions.h).
    ///
    /// The species slot may not change, and a change is REFUSED rather than applied: a species
    /// block is a contiguous run over the sorted array, and moving an instance between blocks would
    /// renumber every slot after it — which would silently rename every instance an exception is
    /// anchored to. A move that changes species is a removal and an addition, and the caller has
    /// both.
    [[nodiscard]] Status replace_instance(u32 slot, const FoliageInstance& instance) noexcept;

    [[nodiscard]] ClusterWind& wind() noexcept { return wind_; }
    [[nodiscard]] const ClusterWind& wind() const noexcept { return wind_; }

    /// The world cell this cluster streams with. Set by `FoliageStreaming` when the cluster is
    /// bound; invalid until then.
    [[nodiscard]] world::CellId cell() const noexcept { return cell_; }
    void set_cell(world::CellId cell) noexcept { cell_ = cell; }

    /// Bytes the cluster's instance storage occupies. What a diagnostic reports and what a test
    /// compares against the instance count.
    [[nodiscard]] u64 bytes() const noexcept;

private:
    friend class ClusterBuilder;

    ClusterId id_;
    ClusterCoord coord_;
    ClusterBounds bounds_;
    Array<FoliageInstance> instances_;
    Array<SpeciesBlock> blocks_;
    Array<SpeciesId> species_;
    ClusterWind wind_;
    world::CellId cell_;
};

inline constexpr SpeciesId kInvalidSpecies{};

/// The cooker policy that decides how big a cluster is. `foliage` — "Cluster size SHALL be a COOKER
/// POLICY balancing culling granularity against per-cluster overhead, and SHALL BE REPORTED rather
/// than fixed as a constant in the specification."
///
/// So it is a value a project sets and `ClusterBuildReport` is the reporting half. Nothing in this
/// module hard-codes an edge length.
struct ClusterPolicy {
    /// Edge of one level-0 cluster, in metres.
    f32 edge_metres = 64.0F;
    /// The instance count the cooker aims at. A cluster far below it is culling granularity spent
    /// on per-cluster overhead; far above it is a cluster that is always partly visible.
    u32 target_instances = 2048;
    /// The hard ceiling. A generation that would exceed it is reported, not silently truncated.
    u32 max_instances = 8192;

    [[nodiscard]] bool is_valid() const noexcept {
        return edge_metres > 0.0F && target_instances > 0 && max_instances >= target_instances;
    }
};

/// What one cluster's build cost and what it dropped. The "SHALL be reported" half of the policy.
struct ClusterBuildReport {
    ClusterId cluster;
    u32 instances = 0;
    u32 species = 0;
    /// Instances the policy's ceiling dropped. Non-zero means the policy is wrong for this world.
    u32 dropped_over_ceiling = 0;
    /// Instances dropped because the cluster already carried `kMaxClusterSpecies` species.
    u32 dropped_species_overflow = 0;
    u64 bytes = 0;
    /// Bytes per instance, as built. The number the "tens of bytes" requirement is judged on.
    [[nodiscard]] f32 bytes_per_instance() const noexcept {
        return instances == 0 ? 0.0F : static_cast<f32>(bytes) / static_cast<f32>(instances);
    }
};

/// Builds one cluster from accepted placements. Separate from `FoliageCluster` because a cluster is
/// immutable once built except for its flags, and because the canonical ORDER an instance's slot
/// depends on is this class's decision and must be in one place.
class ClusterBuilder {
public:
    ClusterBuilder(Allocator& allocator, const ClusterPolicy& policy, ClusterId id,
                   ClusterCoord coord, const ClusterBounds& bounds) noexcept;

    /// Add one placement in absolute coordinates. Accepted in any order; `finish()` imposes the
    /// canonical one.
    [[nodiscard]] Status add(SpeciesId species, const world::WorldVec3d& position, f32 yaw_radians,
                             f32 scale_unit, u8 variation, u8 age, f32 tilt_x_radians,
                             f32 tilt_z_radians, InstanceFlags flags) noexcept;

    /// Sort into the canonical order and produce the cluster.
    ///
    /// **THE CANONICAL ORDER IS WHAT MAKES A SLOT STABLE**, and a slot is what an identity is
    /// derived from. Instances are ordered by species, then by quantised position (z, then x, then
    /// y), then by the remaining bytes — a total order over the STORED record, so two machines that
    /// accepted the same set in different orders write the same cluster. An order that was "the
    /// order placement accepted them" would make every identity depend on a traversal, which is
    /// exactly the scheme the spike measured at 43% mis-bound.
    [[nodiscard]] Expected<FoliageCluster, Error> finish() noexcept;

    [[nodiscard]] const ClusterBuildReport& report() const noexcept { return report_; }
    [[nodiscard]] usize size() const noexcept { return staged_.size(); }

private:
    struct Staged {
        SpeciesId species;
        FoliageInstance instance;
    };

    [[nodiscard]] Expected<u8, Error> slot_for(SpeciesId species) noexcept;

    Allocator* allocator_;
    ClusterPolicy policy_;
    ClusterId id_;
    ClusterCoord coord_;
    ClusterBounds bounds_;
    Array<Staged> staged_;
    Array<SpeciesId> species_;
    ClusterBuildReport report_;
};

// --- Hierarchical culling
// -------------------------------------------------------------------------

/// A view, for culling. A sphere and a plane set would duplicate `Frustum`, which lives in
/// `core-math` and which this module deliberately does not drag a whole render view in for: culling
/// here is the CLUSTER half, the instance half happens on the GPU, and what a cluster test needs is
/// a centre, a radius and a set of planes the caller already has.
struct CullView {
    world::WorldVec3d eye;
    /// Six planes as (nx, ny, nz, d), a point inside satisfying n.p + d >= 0. Absolute space.
    f64 planes[6][4] = {};
    u32 plane_count = 0;
    /// Beyond this, a cluster is not drawn at all. The budget's distance lever writes it.
    f32 max_distance_metres = 4096.0F;
    /// Projected pixels per metre at one metre, for the tier selection. Derived from the view's own
    /// projection by the caller, because a foliage module that computed a projection would be a
    /// foliage module with a camera model in it.
    f32 pixels_per_metre = 1000.0F;
};

/// What a cull produced, and what it cost. `foliage` — "clusters SHALL be culled before instances,
/// so MOST INSTANCES ARE REJECTED IN BULK": `instances_rejected_in_bulk` against
/// `instances_visible` is that sentence as a measurement.
struct CullResult {
    u32 clusters_tested = 0;
    u32 clusters_visible = 0;
    u32 instances_visible = 0;
    /// Instances inside clusters the cluster test rejected — never individually examined.
    u32 instances_rejected_in_bulk = 0;
};

/// One visible cluster and the tier it draws at.
struct VisibleCluster {
    ClusterId cluster;
    DetailTier tier = DetailTier::Detailed;
    f32 distance_metres = 0.0F;
    u32 instances = 0;
};

/// Every cluster in the world, and the hierarchical cull over them.
class ClusterStore {
public:
    explicit ClusterStore(Allocator& allocator) noexcept;

    ClusterStore(const ClusterStore&) = delete;
    ClusterStore& operator=(const ClusterStore&) = delete;

    [[nodiscard]] Status insert(FoliageCluster cluster) noexcept;
    /// Drop one cluster. `foliage` — clusters "SHALL be INDEPENDENTLY EVICTABLE".
    [[nodiscard]] bool evict(ClusterId cluster) noexcept;

    [[nodiscard]] FoliageCluster* find(ClusterId cluster) noexcept;
    [[nodiscard]] const FoliageCluster* find(ClusterId cluster) const noexcept;
    [[nodiscard]] usize size() const noexcept { return clusters_.size(); }
    [[nodiscard]] u64 instance_count() const noexcept;
    [[nodiscard]] u64 bytes() const noexcept;

    /// Clusters in insertion order. A diagnostic's iteration; a cull uses `cull()`.
    [[nodiscard]] Span<const FoliageCluster> clusters() const noexcept { return clusters_.span(); }

    /// The hierarchical cull. Clusters first, in bulk; nothing here examines an instance.
    [[nodiscard]] Expected<CullResult, Error> cull(const SpeciesLibrary& library,
                                                   const CullView& view,
                                                   Array<VisibleCluster>& out) const noexcept;

private:
    Array<FoliageCluster> clusters_;
    HashMap<u64, usize> index_;
};

}  // namespace cy::foliage

namespace cy {

template <>
struct Hash<foliage::ClusterId> {
    [[nodiscard]] u64 operator()(foliage::ClusterId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

template <>
struct Hash<foliage::InstanceId> {
    [[nodiscard]] u64 operator()(foliage::InstanceId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
