#pragma once
// Deterministic submission order. Task 4.1.5, design.md §6.
//
// ================================================================================================
// THE REQUIREMENT, AND WHY IT LOOKS LIKE PEDANTRY UNTIL M9
// ================================================================================================
//
// `rendering-architecture` — "Deterministic submission order": "Draw submission order SHALL be
// determined by explicit sort keys, not by ECS iteration order or thread timing, so a frame is
// reproducible", and its scenario is "WHEN the same snapshot is rendered twice THEN the recorded
// command stream SHALL be identical."
//
// design.md §6: "Draw ordering derives from a sort key computed from stable inputs — material,
// mesh, depth — and never from iteration order over a hash map, pointer values, or the order
// instances happened to be published. At M3 this looks like pedantry. At M9 it is the difference
// between a golden-image test that reproduces and one that is flaky for reasons nobody can find."
//
// ================================================================================================
// WHAT MAKES THIS ACTUALLY DETERMINISTIC, RATHER THAN USUALLY DETERMINISTIC
// ================================================================================================
//
// A sort key is not enough on its own. Two draws with equal keys are ordered by whatever the sort
// does with ties, and every fast sort is unstable — so "equal keys" means "order decided by the
// input order", which is the GPU scene's slot order, which is free-list order, which is allocation
// history. That is exactly the "publication order" the requirement forbids, and it would be
// invisible: the frame is correct, it just is not the *same* frame twice.
//
// So the ordering here is over a TRIPLE — `(key, stable_id, surface)` — where `stable_id` is a
// property of the thing being drawn rather than of where it landed (see `GpuInstance::stable_id`),
// and `surface` distinguishes the several draws one instance produces when its mesh has several
// material slots. Two draws can share a key; two draws cannot share a `(stable_id, surface)` pair,
// and `sort_draws()` says so with an assertion rather than hoping. The comparison is therefore a
// TOTAL order, and a total order has exactly one sorted sequence — which makes the result
// independent of the algorithm, of the input order, and of how many threads produced the input.
//
// ================================================================================================
// THE KEY LAYOUT, AND THE TRADE IT ENCODES
// ================================================================================================
//
//   bits 63..61   sort layer      background, opaque, masked, transparent, overlay
//   bits 60..45   pipeline        the compiled program: the most expensive state change there is
//   bits 44..29   material        the instance's parameter set: the next most expensive
//   bits 28..13   mesh            adjacent draws sharing one mesh are what automatic instancing
//                                 merges, so the mesh must be contiguous inside a material
//   bits 12..0    depth           front-to-back for opaque, back-to-front for transparent
//
// DEPTH LAST FOR OPAQUE IS A DECISION AND NOT AN OVERSIGHT. Sorting opaque draws front to back buys
// early-Z rejection; sorting them by pipeline buys fewer state changes. They are in tension, and
// the tie is broken by the frame having a DEPTH PREPASS (task 4.3.2): after the prepass the opaque
// pass tests `Equal` against depth that is already correct, so front-to-back ordering buys nothing
// the prepass has not already bought, and state sorting is left to win. A pipeline without a
// prepass would want the two swapped, and the swap is `make_sort_key`'s alone.
//
// DEPTH FIRST FOR TRANSPARENT IS NOT A DECISION. Blending is not commutative; back-to-front is
// correctness, not performance, and no amount of state sorting is worth a wrong pixel.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

namespace cy::render {

/// Field widths, published because a test asserts on them and because the two callers that pack a
/// key — this header and any pipeline that wants a different trade — must agree.
inline constexpr u32 kSortLayerBits = 3;
inline constexpr u32 kSortPipelineBits = 16;
inline constexpr u32 kSortMaterialBits = 16;
inline constexpr u32 kSortMeshBits = 16;
inline constexpr u32 kSortDepthBits = 13;

static_assert(kSortLayerBits + kSortPipelineBits + kSortMaterialBits + kSortMeshBits +
                      kSortDepthBits ==
                  64,
              "the sort key is exactly 64 bits: a field that grows must take from another");
static_assert((1U << kSortLayerBits) >= kSortLayerCount,
              "every sort layer must be representable in the key's most significant field");

/// What a draw's key is computed from. Every member is a property of the thing being drawn or of
/// the view looking at it — never of an index, an iteration or a pointer.
struct DrawKeyInputs {
    SortLayer layer = SortLayer::Opaque;
    /// The compiled program's identity. Truncated to `kSortPipelineBits`: two pipelines that
    /// collide sort adjacently, which costs a state change and never costs correctness.
    u32 pipeline = 0;
    u32 material = 0;
    u32 mesh = 0;
    /// Distance from the view along its forward axis, in world units. Non-negative; a draw behind
    /// the camera has been culled before it gets here, and a negative value is clamped to zero
    /// rather than wrapping the quantisation.
    f32 view_depth = 0.0F;
};

/// Quantise a non-negative distance into `kSortDepthBits`, monotonically.
///
/// The trick is worth stating because it looks like a hack and is not: for non-negative IEEE-754
/// floats, the bit pattern read as an unsigned integer is monotonic in the value. Taking the top
/// bits of that pattern is therefore a monotone quantisation with NO range parameter — which
/// matters, because the alternative is dividing by a far plane, and the engine's default far plane
/// is infinite. It also distributes resolution logarithmically, which is what a depth sort wants:
/// the difference between 1 m and 2 m matters and the difference between 900 m and 901 m does not.
[[nodiscard]] u32 quantise_depth(f32 view_depth) noexcept;

/// Pack a key. Front-to-back within a layer for the opaque layers, back-to-front for transparent —
/// decided by the layer rather than by the caller, so a transparent draw cannot be submitted in the
/// wrong direction by passing the wrong flag.
[[nodiscard]] u64 make_sort_key(const DrawKeyInputs& inputs) noexcept;

/// The fields of a key, read back.
///
/// Automatic instancing reads them (mesh.h): draws that can merge are adjacent because the key put
/// them there, so the merge tests the same fields the sort used rather than re-deriving them from
/// the instance records — which is what keeps "sorted" and "mergeable" from drifting apart.
[[nodiscard]] SortLayer sort_key_layer(u64 key) noexcept;
[[nodiscard]] u32 sort_key_pipeline(u64 key) noexcept;
[[nodiscard]] u32 sort_key_material(u64 key) noexcept;
[[nodiscard]] u32 sort_key_mesh(u64 key) noexcept;

/// One submitted draw. Deliberately small — three words — because a frame sorts tens of thousands
/// of them and the sort's cost is memory traffic.
struct DrawItem {
    u64 key = 0;
    /// The tie-break, and the reason the order is total. See the header comment.
    u64 stable_id = 0;
    /// Where the instance's record lives in the GPU scene. Carried through the sort and NEVER
    /// compared: it is allocation order, which is the thing being kept out of the ordering.
    u32 instance_slot = 0;
    /// The mesh surface within the instance's mesh. One instance with three material slots is three
    /// draws, and they differ only here.
    u32 surface = 0;
};

/// Order `draws` by `(key, stable_id, surface)`.
///
/// In a development build this asserts that no two items share a `(stable_id, surface)` pair — a
/// duplicate makes the order depend on the input again, silently, and it is the one defect this
/// file cannot detect after the fact. The assertion is compiled out of Profile and Shipping, where
/// the cost of the check is not worth paying every frame; the suite that covers it runs in Debug.
void sort_draws(Span<DrawItem> draws) noexcept;

/// Whether `draws` is ordered. What a test asserts, and what a `--validate-frame` switch checks
/// without re-sorting.
[[nodiscard]] bool draws_are_ordered(Span<const DrawItem> draws) noexcept;

/// A 64-bit fingerprint of a submission order: every key, stable id, slot and surface, folded in
/// sequence.
///
/// THIS IS WHAT "the recorded command stream SHALL be identical" IS ASSERTED WITH. Comparing two
/// frames' draw lists element by element requires holding both; comparing two numbers does not, so
/// a golden test can record one number and a replay can check it. It is the same role
/// `CompiledGraph::plan_hash` plays for the graph, and the two together cover task 7.6.
[[nodiscard]] u64 submission_fingerprint(Span<const DrawItem> draws) noexcept;

}  // namespace cy::render
