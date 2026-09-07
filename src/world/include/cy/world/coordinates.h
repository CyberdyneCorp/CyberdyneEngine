#pragma once
// CyberWorld's coordinates and its cell identity. Tasks 3.1 and 3.2.
//
// `world-partition-and-streaming` — "World coordinates": the authoritative persistent form of a
// position is CELL-RELATIVE, a cell coordinate plus a 32-bit float local offset. That is the form
// used for persistence, networking and spatial indexing; the 64-bit accessor below exists for
// tooling and interchange and is NOT the runtime representation.
//
// WHY THIS IS NOT A DOUBLE. A f32 has 24 bits of mantissa, so at 1 000 km from the origin its
// spacing is 64 mm and a character jitters. Cell-relative coordinates never grow: the local offset
// is bounded by the cell's size, so its precision is the same at the origin and a thousand
// kilometres away. `core-math`'s precision policy says the engine does not go double-precision, and
// this is the mechanism that lets it keep saying so.
//
// --- CELL IDENTITY IS A HASH, AND THAT IS DELIBERATE --------------------------------------------
//
// `world-partition-and-streaming` — "Stable cell identity": the identifier encodes the partition,
// the hierarchy level and the spatial position, is generated deterministically from the
// partitioner's configuration, and is OPAQUE to consumers. Save games, the network protocol,
// patching, streaming caches and build caches all key on it.
//
// A packed bitfield would satisfy the first half and fail the second: the moment a consumer can see
// that bits 0..19 are x, some consumer will do arithmetic on it, and the partition configuration is
// then no longer free to change shape. So the identifier is a 64-bit mix of the partition
// signature, the level and the three coordinates, and the signature is what makes "changing cell
// size changes cell identity" true by construction rather than by a rule somebody has to remember.
// `PartitionConfig::signature()` is the value a world asset records so that the change can be
// REPORTED rather than silently producing incompatible data.
//
// THE HASH IS FIXED-SEED. `cy::hash_bytes()` with the process seed is randomised in development
// builds on purpose (core/memory/hash.h), and a cell identifier that changed between two runs of
// the cooker would be the opposite of stable. Every hash in this file passes `kWorldHashSeed`.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/hash.h>

namespace cy::world {

/// The seed every identity hash in CyberWorld uses. A constant, never `hash_seed()`: an identifier
/// that changes between two runs of one cooker is not an identifier.
inline constexpr u64 kWorldHashSeed = 0x6379'6265'7277'6C64ull;  // "cybe" "rwld"

/// The coarsest hierarchy level a partitioner may declare. Four levels at a ratio of four span a
/// factor of 64 in cell size, which is the half-metre prop and the kilometre landform the
/// specification's "scales are not forced together" scenario names.
inline constexpr u8 kMaxPartitionLevels = 8;

/// A cell's place in the hierarchy: integer coordinates at one level. Level 0 is the FINEST.
struct CellCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(const CellCoord&, const CellCoord&) noexcept = default;
};

/// An opaque, stable cell identifier. Consumers compare and hash it; nothing else.
struct CellId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

    friend constexpr bool operator==(CellId, CellId) noexcept = default;
    /// Ordered so that a plan, a report and a cook manifest can be sorted into one canonical order.
    /// The order is arbitrary and stable, which is exactly what determinism needs of it.
    friend constexpr bool operator<(CellId a, CellId b) noexcept { return a.value < b.value; }
};

/// The 64-bit accessor. Tooling, geodetic work and interchange — never the runtime form.
struct WorldVec3d {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

/// The partition's configuration. Its `signature()` is a contribution to every cell identifier, so
/// two of these that differ in any field name different cells for the same point in space.
struct PartitionConfig {
    /// Distinguishes two partitions of one world — a runtime partition and a navigation partition,
    /// say — so their identifiers cannot collide.
    u32 partition = 0;
    /// The size of a level-0 cell, in metres.
    f32 base_cell_size = 128.0f;
    /// How many levels the hierarchy has, including level 0.
    u8 levels = 3;
    /// The factor by which a level's cell size exceeds the level below it.
    u32 level_ratio = 4;
    /// Where the level-0 grid's origin sits in absolute space.
    WorldVec3d origin;

    [[nodiscard]] f64 cell_size(u8 level) const noexcept;
    /// Deterministic over the fields above. Fixed-seed, so it is the same on every machine.
    [[nodiscard]] u64 signature() const noexcept;
    /// Rejects a configuration that cannot name cells: no levels, a non-positive cell size, a ratio
    /// below two, or a hierarchy taller than `kMaxPartitionLevels`.
    [[nodiscard]] bool is_valid() const noexcept;
};

/// The cell identifier for a coordinate under a configuration. Deterministic, opaque, and different
/// for every field of the configuration.
[[nodiscard]] CellId cell_id_of(const PartitionConfig& config, CellCoord coord) noexcept;

/// The persistent form of a position: which cell, and where inside it.
///
/// `local` is bounded by the cell's size once `normalized()` has been applied. Nothing enforces the
/// bound on a freshly-assigned value, because a system that has just moved an entity past a
/// boundary holds an out-of-range offset for exactly as long as it takes to rebase it.
struct WorldPosition {
    CellCoord cell;
    Vec3 local;
};

/// Absolute position, for tooling. The f64 arithmetic is the point of the function.
[[nodiscard]] WorldVec3d to_absolute(const PartitionConfig& config,
                                     const WorldPosition& position) noexcept;

/// The cell-relative form of an absolute position at a level.
[[nodiscard]] WorldPosition from_absolute(const PartitionConfig& config, const WorldVec3d& absolute,
                                          u8 level) noexcept;

/// Rebase so that `local` lies within [0, cell_size). The identity of the position is unchanged;
/// which cell holds it may not be. This is what a moving entity's tracker calls at a boundary.
[[nodiscard]] WorldPosition normalized(const PartitionConfig& config,
                                       const WorldPosition& position) noexcept;

/// The absolute origin physics, rendering and audio use for a region. `world-partition-and-
/// streaming` — "the engine SHALL define the origin used by physics, rendering and audio for a
/// given region" — and this is that definition: the minimum corner of the region's cell.
[[nodiscard]] WorldVec3d simulation_origin(const PartitionConfig& config,
                                           CellCoord region) noexcept;

/// A persistent position expressed relative to a simulation origin. f32, because the whole point of
/// the cell-relative form is that this subtraction is exact.
[[nodiscard]] Vec3 to_simulation_local(const PartitionConfig& config, const WorldPosition& position,
                                       CellCoord region) noexcept;

/// The axis-aligned bounds of a cell, in absolute space, as f32. Bounded worlds only — a cell a
/// thousand kilometres out is representable but its bounds carry the f32 spacing at that distance,
/// which is why the streaming planner works in `WorldVec3d` and uses this only for reporting.
[[nodiscard]] Aabb cell_bounds(const PartitionConfig& config, CellCoord coord) noexcept;

}  // namespace cy::world

namespace cy {

template <>
struct Hash<world::CellId> {
    [[nodiscard]] u64 operator()(world::CellId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

template <>
struct Hash<world::CellCoord> {
    /// Field by field rather than over the object's bytes: `CellCoord` has three bytes of padding
    /// after `level`, and hashing padding reads memory nobody wrote.
    [[nodiscard]] u64 operator()(const world::CellCoord& coord) const noexcept {
        u64 value = hash_integer(static_cast<u64>(static_cast<u32>(coord.x)), hash_seed());
        value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.y)));
        value = hash_combine(value, static_cast<u64>(static_cast<u32>(coord.z)));
        return hash_combine(value, coord.level);
    }
};

}  // namespace cy
