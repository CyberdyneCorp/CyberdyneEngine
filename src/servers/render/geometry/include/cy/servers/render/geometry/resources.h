#pragma once
// GPU resource residency, lifetime and the memory ledger; mip streaming; dynamic geometry. M6 task
// 8.4.
//
// `rendering-geometry-and-resources` — three requirements this file closes, and one it reports on:
//
//   * "Resource residency and lifetime": "GPU resources SHALL be reference counted through their
//     asset handles and released only after the GPU has finished all frames that could reference
//     them. The engine SHALL report GPU memory by category (textures, meshes, render targets,
//     buffers, acceleration structures) and support a memory budget with eviction of streamable
//     content."
//   * "Texture streaming": mip streaming is "a first-class model for ordinary assets", holding "a
//     residency budget from the memory budget tree", prioritising "through the shared residency
//     policy", never blocking the frame — "a non-resident mip SHALL fall back to the highest
//     resident one" — and keeping "the lowest few mips always resident so no texture is ever
//     entirely missing".
//   * "Procedural and dynamic geometry": dynamic meshes with ring-buffered storage, immediate-mode
//     geometry, and a mesh builder.
//   * "Mesh and texture diagnostics": the counts, the LOD level in use, and the mip-level heat map.
//
// ================================================================================================
// DEFERRED RELEASE IS THE POINT, AND IT IS ONE COUNTER
// ================================================================================================
//
// "released only after the GPU has finished all frames that could reference them" is the single
// most commonly got-wrong rule in a renderer, because the wrong version — release when the
// reference count reaches zero — is correct on a CPU and works in testing on a GPU.
// `ResourceLedger` therefore never frees at the moment a count reaches zero: it records the frame
// in which that happened and hands the resource back only when `retire(frame)` is called with a
// frame index the device has finished. A caller that never calls `retire` leaks, visibly, in the
// report — which is a better failure than a use-after-free that appears once in a thousand frames.
//
// ================================================================================================
// THIS MODULE HOLDS POLICY NUMBERS, NOT PAGES
// ================================================================================================
//
// `residency` is the shared policy and, at M6, its own server at layer 2. What is here is the
// GEOMETRY-side half: which categories exist, what each currently costs, what a texture's resident
// mip range is, and which mip a sampler would land on. The decision about what to evict under
// pressure belongs to the shared policy, and `evict_candidates` produces the ordered list that
// policy chooses from rather than choosing itself — which is the separation the specification means
// by "eviction SHALL follow the shared residency policy rather than a texture-specific rule".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

namespace cy::render::geometry {

// --- The memory ledger --------------------------------------------------------------------------

/// The categories a GPU memory report is broken down by. The specification's list, verbatim, plus
/// the skinning output buffers, which are none of the five and are large.
enum class MemoryCategory : u8 {
    Textures = 0,
    Meshes,
    RenderTargets,
    Buffers,
    AccelerationStructures,
    /// Double-buffered skinned vertices. See skinning.h.
    SkinnedVertices,
    Count,
};

inline constexpr u32 kMemoryCategoryCount = static_cast<u32>(MemoryCategory::Count);

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* memory_category_name(MemoryCategory category) noexcept;

/// One resource the ledger tracks.
using ResourceId = u32;
inline constexpr ResourceId kInvalidResource = ~0U;

/// Per-category totals, as a report reads them.
struct MemoryReport {
    u64 bytes[kMemoryCategoryCount] = {};
    u64 budget[kMemoryCategoryCount] = {};
    u32 resources[kMemoryCategoryCount] = {};
    /// Bytes whose reference count has reached zero and which the device has not yet finished with.
    /// A number that grows without bound is a `retire` that is never called.
    u64 pending_release = 0;
    /// Bytes reclaimed since the ledger was created.
    u64 released = 0;
    /// How many times a category was over its budget when a resource was added, which is what a
    /// budget is for reporting rather than for refusing: refusing an allocation mid-frame produces
    /// a missing object, and the requirement asks for eviction instead.
    u64 budget_exceeded = 0;

    [[nodiscard]] u64 total_bytes() const noexcept;
};

/// Reference-counted GPU resources with deferred release and a per-category budget.
///
/// Holds no device handle: a `ResourceId` is this ledger's own index, and the module above maps it
/// to whatever the RHI called the thing. That is what lets every case in tests/ run with no GPU.
class ResourceLedger {
public:
    explicit ResourceLedger(Allocator& allocator) noexcept;

    ResourceLedger(const ResourceLedger&) = delete;
    ResourceLedger& operator=(const ResourceLedger&) = delete;

    /// How many frames the device may still be reading a resource in. A release recorded in frame
    /// N is reclaimed by `retire(N + frames_in_flight)`.
    void set_frames_in_flight(u32 frames) noexcept { frames_in_flight_ = frames; }
    [[nodiscard]] u32 frames_in_flight() const noexcept { return frames_in_flight_; }

    [[nodiscard]] Status set_budget(MemoryCategory category, u64 bytes) noexcept;

    /// Register a resource with one reference. `streamable` marks content eviction may reclaim —
    /// a streamed texture's upper mips, a mesh whose asset can be re-read — as against a render
    /// target, which cannot be evicted because nothing can produce it again on demand.
    [[nodiscard]] Expected<ResourceId, Error> acquire(MemoryCategory category, u64 bytes,
                                                      bool streamable) noexcept;

    /// Take another reference. Fails on an unknown or already-retired id.
    [[nodiscard]] Status add_reference(ResourceId id) noexcept;

    /// Drop a reference, recording `frame` as the frame in which it reached zero. The bytes stay
    /// charged to their category until `retire`.
    [[nodiscard]] Status release(ResourceId id, u64 frame) noexcept;

    /// Reclaim everything released in a frame the device has finished. Returns the bytes reclaimed.
    ///
    /// "WHEN a level is unloaded THEN its GPU resources SHALL be released after the in-flight
    /// frames complete, and the reclaimed memory SHALL be reported."
    u64 retire(u64 completed_frame) noexcept;

    [[nodiscard]] u32 reference_count(ResourceId id) const noexcept;
    [[nodiscard]] bool live(ResourceId id) const noexcept;
    [[nodiscard]] u64 bytes_of(ResourceId id) const noexcept;

    /// The streamable resources of a category that is over budget, largest first, so the shared
    /// residency policy has an ordered list to choose from. Returns how many were written.
    ///
    /// Largest first is an ORDERING and not a decision: this module does not know an instance's
    /// importance, and `residency` does. A ledger that evicted by size alone would drop the terrain
    /// the camera is standing on because it is big.
    [[nodiscard]] u32 evict_candidates(MemoryCategory category,
                                       Span<ResourceId> out) const noexcept;

    [[nodiscard]] MemoryReport report() const noexcept;

private:
    struct Entry {
        u64 bytes = 0;
        u64 released_frame = 0;
        u32 references = 0;
        MemoryCategory category = MemoryCategory::Buffers;
        bool streamable = false;
        bool live = false;
        bool pending = false;
    };

    Array<Entry> entries_;
    Array<ResourceId> free_;
    u64 budget_[kMemoryCategoryCount] = {};
    u64 released_ = 0;
    u64 budget_exceeded_ = 0;
    u32 frames_in_flight_ = 2;
};

// --- Mip streaming ------------------------------------------------------------------------------

/// How a texture is resident. `virtual-texturing` governs the two virtual models and this module
/// implements the two it owns.
enum class TextureResidency : u8 {
    /// Every mip, always. A user-interface texture, a lookup table.
    FullyResident = 0,
    /// Partial residency by mip level, driven by renderer feedback and by distance where feedback
    /// is
    /// unavailable. "It is simpler and cheaper than virtual texturing for the majority of a
    /// project's textures and SHALL NOT be treated as a legacy path."
    MipStreamed = 1,
    /// `virtual-texturing`'s. Declared so this enumeration is the whole set a cooked texture may
    /// name, and rejected by `MipChain::configure` because this module does not implement it.
    VirtualStreamed = 2,
    VirtualRuntime = 3,
};

[[nodiscard]] const char* texture_residency_name(TextureResidency residency) noexcept;

/// One streamed texture's mip residency.
///
/// THE MIP TAIL IS A GUARANTEE, NOT A HEURISTIC. "The lowest few mips SHALL always be resident so
/// no texture is ever entirely missing", so `configure` refuses a tail of zero and `resident_mip`
/// never answers a level below the tail's top. A frame is therefore never missing a texture — only
/// sampling a coarser one, which is the degradation axis every paged system in this engine
/// declares.
class MipChain {
public:
    /// `mip_count` is the full chain; `tail` is how many of the smallest are pinned resident.
    ///
    /// Refuses `VirtualStreamed` and `VirtualRuntime`, which belong to `virtual-texturing`, and a
    /// tail of zero, which is the guarantee above.
    [[nodiscard]] Status configure(TextureResidency residency, u8 mip_count, u8 tail,
                                   u64 bytes_of_mip_zero) noexcept;

    /// Ask for mips down to `level` (0 is the largest). Records the request; residency changes only
    /// when `commit_resident` is called with what actually arrived.
    void request(u8 level) noexcept;

    /// Record what is now resident: mips `level` and smaller.
    void commit_resident(u8 level) noexcept;

    /// The level a sampler actually reads when it asks for `wanted`.
    ///
    /// "WHEN the camera approaches and higher mips are sampled THEN they SHALL be scheduled and
    /// swapped in when ready, with the lower mip shown meanwhile" — so a request for a level that
    /// is not resident answers the highest resident one rather than blocking or failing.
    [[nodiscard]] u8 resident_mip(u8 wanted) const noexcept;

    [[nodiscard]] u8 mip_count() const noexcept { return mip_count_; }
    [[nodiscard]] u8 tail() const noexcept { return tail_; }
    [[nodiscard]] u8 highest_resident() const noexcept { return highest_resident_; }
    [[nodiscard]] u8 requested() const noexcept { return requested_; }
    [[nodiscard]] TextureResidency residency() const noexcept { return residency_; }

    /// Bytes a residency down to `level` costs, assuming a halving chain.
    [[nodiscard]] u64 bytes_for(u8 level) const noexcept;
    [[nodiscard]] u64 resident_bytes() const noexcept { return bytes_for(highest_resident_); }
    /// What the current request would cost if it were satisfied. The number a budget is checked
    /// against before a stream is scheduled.
    [[nodiscard]] u64 requested_bytes() const noexcept { return bytes_for(requested_); }

    /// True while the request has not been satisfied — what a scheduler iterates.
    [[nodiscard]] bool pending() const noexcept { return requested_ < highest_resident_; }

private:
    u64 bytes_of_mip_zero_ = 0;
    TextureResidency residency_ = TextureResidency::FullyResident;
    u8 mip_count_ = 1;
    u8 tail_ = 1;
    /// The smallest-numbered — largest — mip currently resident.
    u8 highest_resident_ = 0;
    u8 requested_ = 0;
};

// --- Dynamic and immediate geometry --------------------------------------------------------------

/// A ring-buffered vertex and index allocator for geometry rebuilt every frame.
///
/// "WHEN a system generates a mesh at runtime THEN it SHALL write into a dynamic mesh whose buffers
/// are ring-buffered across frames in flight."
///
/// The ring is what makes that safe without a fence per allocation: the buffer is divided into as
/// many slices as there are frames in flight, a frame writes only its own slice, and a slice is
/// reused only after the device has finished the frame that last wrote it. An allocation that does
/// not fit the remaining slice FAILS rather than wrapping into the next one, because wrapping is
/// how a frame overwrites the vertices the device is still reading.
class GeometryRing {
public:
    GeometryRing() noexcept = default;

    /// `slice_bytes` is the per-frame budget and `frames` the number of slices.
    [[nodiscard]] Status configure(u64 slice_bytes, u32 frames) noexcept;

    /// Begin a frame, resetting its slice. `frame` selects the slice by parity.
    void begin_frame(u64 frame) noexcept;

    /// Reserve `bytes` at `alignment` within the current slice. Returns the offset into the whole
    /// buffer, so a caller binds one buffer and offsets into it.
    [[nodiscard]] Expected<u64, Error> allocate(u64 bytes, u64 alignment) noexcept;

    [[nodiscard]] u64 used_this_frame() const noexcept { return used_; }
    [[nodiscard]] u64 slice_bytes() const noexcept { return slice_bytes_; }
    [[nodiscard]] u64 total_bytes() const noexcept { return slice_bytes_ * frames_; }
    /// The high-water mark across every frame since configuration. What sizes the ring next time.
    [[nodiscard]] u64 peak() const noexcept { return peak_; }
    /// How many allocations were refused because the slice was full. A number above zero is a ring
    /// that is too small, and it is reported rather than being papered over by a wrap.
    [[nodiscard]] u64 overflows() const noexcept { return overflows_; }

private:
    u64 slice_bytes_ = 0;
    u64 used_ = 0;
    u64 peak_ = 0;
    u64 overflows_ = 0;
    u32 frames_ = 0;
    u32 slice_ = 0;
};

// --- Diagnostics ---------------------------------------------------------------------------------

/// `rendering-geometry-and-resources` — "Mesh and texture diagnostics": "triangle and vertex counts
/// per mesh and per frame, LOD level in use, texture resolution and streaming state (a mip-level
/// heat map), overdraw, and vertex bandwidth per pass".
struct GeometryStatistics {
    u64 triangles = 0;
    u64 vertices = 0;
    u32 draws = 0;
    /// How many draws used each level. Index 7 accumulates everything past 6.
    u32 lod_histogram[8] = {};
    /// How many sampled textures were resident at each mip level. The heat map, as a histogram: the
    /// scenario is "the mip-level debug view shows a texture never sampling above mip 4", and the
    /// number that shows it is this one.
    u32 mip_histogram[16] = {};
    /// Bytes of vertex data a pass fetched, which is what "vertex bandwidth per pass" means and
    /// what makes the case for a stream split visible.
    u64 vertex_bytes = 0;
    /// Fragments shaded divided by pixels covered, times 256, so overdraw is an integer a GPU
    /// counter can accumulate. 256 is exactly one.
    u32 overdraw_fixed_point = 0;

    void clear() noexcept;
    /// Fold another frame's or another view's numbers in.
    void merge(const GeometryStatistics& other) noexcept;
};

}  // namespace cy::render::geometry
