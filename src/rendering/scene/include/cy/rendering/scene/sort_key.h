#pragma once
// Draw sort keys, and the reason they are built from content rather than from addresses.
// Task 4.1.5, design.md §6.
//
// `rendering-architecture` — "Deterministic submission order": "Draw submission order SHALL be
// determined by explicit sort keys, not by ECS iteration order or thread timing, so a frame is
// reproducible", and the scenario is that rendering one snapshot twice records an identical command
// stream.
//
// ================================================================================================
// THE THREE SOURCES OF ORDER THAT LOOK STABLE AND ARE NOT
// ================================================================================================
//
// Each of these is available, cheap, already sorted, and wrong. They are written out because every
// one of them is what a reasonable person reaches for first.
//
//   A HANDLE VALUE, OR A GPU-SCENE SLOT INDEX. Both are allocation order, which is publication
//     order. Publish the same scene after a different history — one entity destroyed last session,
//     a producer registered in a different order, a level streamed in a different sequence — and
//     the same content lands in different slots. design.md §6 names this one directly.
//
//   A POINTER. Address-space layout randomisation makes it differ between two runs of the *same*
//     binary on the same machine.
//
//   THE PROCESS HASH SEED. `cy::hash_seed()` is deliberately randomised per process in development
//     builds, which is what makes hash-order dependencies fail loudly instead of silently. A sort
//     id derived from it would reorder draws between two runs of the same frame — the exact defect
//     this file exists to prevent, introduced by the mechanism meant to catch it. Everything here
//     therefore hashes with `kSortSeed`, a fixed constant, and never with the process seed.
//
// What is left is content: the material program's identity, the mesh's identity, the surface index,
// the view-space depth, and a stable per-instance identity the producer supplies. All five survive
// a restart, a different streaming order and a different machine.
//
// ================================================================================================
// THE KEY LAYOUT, AND WHY THERE ARE TWO OF THEM
// ================================================================================================
//
// Opaque draws sort by state first and depth second, because binding a pipeline is expensive and
// front-to-back only has to be approximate for early-Z to work. Transparent draws sort by depth
// first and state second, because back-to-front is a correctness requirement and batching is not.
// One key layout cannot do both, so there are two builders and one comparison.
//
//   opaque       [63:61] layer  [60:45] program  [44:29] mesh  [28:5] depth (near first)  [4:0]
//   surface transparent  [63:61] layer  [60:37] depth (far first)  [36:21] program  [20:5] mesh
//   [4:0] surface
//
// `program` and `mesh` are 16-bit folds of 64-bit stable identities. A fold collides, and a
// collision costs a lost batch — two materials that could have been drawn together are not
// adjacent. It never costs correctness, and it never costs determinism, because the full 64-bit
// identities go into the tiebreak below.
//
// ================================================================================================
// WHY A SECOND WORD
// ================================================================================================
//
// A 64-bit key is not injective, and `std::sort` is not stable. Two draws with equal keys would
// therefore come out in an order that depends on their input order — which is publication order,
// which is the thing being avoided. `DrawSortKey` is consequently a pair: the bucketing word above,
// and a `tiebreak` word carrying the full stable identity of the draw. The comparison is
// lexicographic over the two, so the ordering is total whenever the identities are distinct, and
// `verify_total_order()` is what says they are.

#include <cy/core/base/types.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>

namespace cy::rendering {

/// The coarsest ordering: which group of draws this one belongs to. Occupies the top three bits, so
/// no depth or state comparison can ever move a draw out of its group.
enum class SortLayer : u8 {
    /// Written before anything else and read by everything: the depth prepass.
    DepthPrepass = 0,
    Opaque = 1,
    /// Alpha-tested. Between opaque and transparent because it is opaque in the depth buffer and
    /// pays a discard in the shader, so grouping it separately keeps the discard out of the opaque
    /// pipeline's specialization.
    Masked = 2,
    Transparent = 3,
    /// Drawn after the scene, unsorted against it: debug geometry, world-space UI overlays.
    Overlay = 4,
};

/// The fixed seed every sort identity is hashed with.
///
/// Not `cy::hash_seed()`. See the header comment — the process seed is randomised in development
/// builds on purpose, and using it here would randomise draw order between runs.
inline constexpr u64 kSortSeed = 0x9E3779B97F4A7C15ULL;

/// A stable 64-bit identity, hashed with `kSortSeed`. The input must be content, not an address.
[[nodiscard]] inline u64 stable_identity(u64 content) noexcept {
    return hash_integer(content, kSortSeed);
}

/// Fold a 64-bit stable identity into the 16 bits the key has room for.
///
/// Both halves are folded in rather than the low bits taken, so two identities differing only above
/// bit 16 still land in different buckets.
[[nodiscard]] inline u16 sort_bucket(u64 identity) noexcept {
    const u64 folded = identity ^ (identity >> 32U);
    return static_cast<u16>((folded ^ (folded >> 16U)) & 0xFFFFU);
}

/// What a draw is sorted by. Filled by the caller from content; never from a handle or a slot.
struct DrawSortInput {
    SortLayer layer = SortLayer::Opaque;
    /// The stable identity of the compiled material program — not of the material *instance*.
    /// Instances of one program share a pipeline, so sorting by the program is what makes the
    /// "Instance shares a pipeline" scenario group them together.
    u64 program_identity = 0;
    u64 mesh_identity = 0;
    /// The identity of the thing being drawn, stable across runs: an entity id, an effect's
    /// particle id, a foliage cluster's cell key. **Not** a GPU-scene slot index.
    u64 instance_identity = 0;
    /// Which surface of the mesh. Meshes have few surfaces; 32 is the addressable maximum and the
    /// builder saturates rather than wrapping.
    u32 surface = 0;
    /// Reversed-Z clip depth, `[0, 1]`, 1 at the near plane. `cy::DepthConvention` is where that
    /// comes from; a caller passing a view-space distance in metres gets a correct but coarse
    /// ordering, and a caller passing a conventional-depth value gets its draws inside out.
    f32 depth = 0.0f;
};

/// The sortable key. Compared lexicographically: bucket first, then full identity.
struct DrawSortKey {
    u64 primary = 0;
    u64 tiebreak = 0;

    friend constexpr bool operator==(DrawSortKey, DrawSortKey) noexcept = default;
    [[nodiscard]] friend constexpr bool operator<(DrawSortKey a, DrawSortKey b) noexcept {
        return a.primary != b.primary ? a.primary < b.primary : a.tiebreak < b.tiebreak;
    }
};

/// Build the key. `layer` decides which of the two layouts is used.
[[nodiscard]] DrawSortKey make_sort_key(const DrawSortInput& input) noexcept;

/// One entry of a draw list: the key, and where the data lives.
///
/// `instance_slot` is the GPU scene slot the draw reads from. It is carried, never compared — see
/// the header comment on why a slot index must not reach the ordering.
struct DrawItem {
    DrawSortKey key;
    u32 instance_slot = 0;
    u32 surface = 0;
};

/// Order a draw list. Deterministic whenever the keys are distinct, which `verify_total_order()`
/// checks and which a producer supplying a stable instance identity guarantees.
void sort_draws(Span<DrawItem> draws) noexcept;

/// The index of the first pair of adjacent equal keys in a sorted list, or `draws.size()` when the
/// order is total.
///
/// Two equal keys mean two draws claiming the same identity, and the consequence is that their
/// relative order is decided by the input order — the flaky golden image design.md §6 is about.
/// Returned rather than asserted so that a caller can name the offending draw; the render server
/// asserts on it in development builds.
[[nodiscard]] usize first_duplicate_key(Span<const DrawItem> draws) noexcept;

[[nodiscard]] inline bool verify_total_order(Span<const DrawItem> draws) noexcept {
    return first_duplicate_key(draws) == draws.size();
}

}  // namespace cy::rendering
