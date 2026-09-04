#pragma once
// The GPU scene: one instance representation, published into by many producers. Task 4.1.4.
//
// ================================================================================================
// THIS IS A PUBLICATION INTERFACE, AND THAT IS THE POINT OF THE FILE
// ================================================================================================
//
// design.md §4: "The GPU scene is defined as a **publication interface** in this milestone, even
// though there is exactly one producer. From M7 onward VFX publishes mesh particles into it,
// animation publishes skinned instances, and virtual geometry publishes clusters — none of them
// through the ECS and none through a CPU round trip."
//
// `rendering-architecture` lists the producers by name — extract, instanced meshes, VFX,
// world-space UI, foliage, terrain and water — and requires that "all producers SHALL use the same
// representation, so downstream culling, LOD, sorting, and drawing require no knowledge of an
// instance's origin".
//
// So the shape below is designed against the producers that are COMING, not the one that exists.
// Three requirements from those specifications drove every decision here, and each is answered in
// the interface rather than in a comment:
//
//   1. "Instance publication SHALL be possible entirely GPU-side, without CPU round trips, for
//      producers whose data already lives on the GPU" (`rendering-architecture`), and "Mesh
//      particles SHALL NOT require ECS entities, per-particle CPU submission, or CPU readback"
//      (`vfx-system`).
//      ANSWER: a producer RESERVES a contiguous slot range once and declares who writes it. A
//      `Residency::Gpu` range is never touched by the CPU and never uploaded: a compute shader
//      writes those records in place. Reservation is the only CPU cost, and it is per effect rather
//      than per particle.
//
//   2. "Instance culling SHALL read the GPU scene, so virtual geometry does not traverse ECS
//      entities or maintain its own instance list" (`virtual-geometry`).
//      ANSWER: the records are one flat, contiguous array addressed by slot index, with no
//      per-producer indirection to chase, and `high_water()` is the dispatch bound. A consumer
//      cannot tell what produced a record and has nowhere to look it up.
//
//   3. "WHEN an effect, entity, or UI document is destroyed THEN its instances SHALL be removed
//   from
//      the GPU scene without requiring a full rebuild" (`rendering-architecture`).
//      ANSWER: `release()` returns the range to the free list and overwrites its records with the
//      inactive pattern — `flags = 0`, `layer_mask = 0` — so a cull that has already been
//      dispatched rejects them and nothing is compacted. Removal costs the size of the range and
//      nothing else.
//
// ================================================================================================
// WHAT IS DELIBERATELY *NOT* HERE
// ================================================================================================
//
// No device, no buffer, no upload. This module is layer 2 and cannot name an RHI type; the GPU-side
// mirror is `cy::rendering::GpuSceneUploader` in src/rendering/scene/, which reads `dirty_ranges()`
// and writes them into a storage buffer. Keeping the representation below the device is what lets a
// producer be tested — and a plan asserted — with no GPU at all, and it is the same split that lets
// the render graph's derivation run in continuous integration.
//
// --- THE SEAM CAMERA-RELATIVE RENDERING WILL USE, NAMED NOW SO IT IS NOT INVENTED TWICE ----------
//
// Transforms below are published in WORLD space, and design.md §3 requires positions to reach the
// GPU relative to the camera. The subtraction happens per view, in the shader, from the view's own
// origin (`View::camera_relative_origin` in model.h) — which is correct for any scene whose
// coordinates survive f32 at publication time.
//
// It is not correct forever. At a million units from the origin an f32 translation resolves to
// about 0.06 units, and that error is baked in HERE, before any view sees it. If task 5.3's
// million-unit scene shows jitter that the per-view subtraction cannot remove, the fix is a scene
// origin on this store, rebased in large steps, with the records holding positions relative to it —
// and the field belongs on `GpuScene`, next to the records, because every producer must agree on
// it. That is a change to this file and to the publication helpers, and to nothing that consumes a
// record.
//
// No sort key and no visibility list. Sorting is sort.h's and culling is the pipeline's; both read
// these records and neither belongs inside the store that holds them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

#include <cstddef>
#include <type_traits>

namespace cy::render {

// --- The record -----------------------------------------------------------------------------

/// Per-instance flags. The low bits are what a GPU cull tests, so they are stable numbers rather
/// than an enum class the shader cannot see.
enum InstanceFlagBits : u32 {
    /// Set for a live record. A slot that has never been reserved, or has been released, has this
    /// clear and is skipped by every consumer without any other bookkeeping.
    kInstanceActive = 1U << 0U,
    kInstanceVisible = 1U << 1U,
    kInstanceCastsShadow = 1U << 2U,
    kInstanceReceivesShadow = 1U << 3U,
    /// The transform changed since the previous frame, so motion vectors are non-zero and a shadow
    /// page that contains it must be refreshed. Written by whoever publishes the transform.
    kInstanceMoved = 1U << 4U,
    /// The mesh is skinned and its vertices come from the skinning pass's output buffer rather than
    /// from the mesh's own. M7's animation producer sets it; M3 never does.
    kInstanceSkinned = 1U << 5U,
    kInstanceTwoSided = 1U << 6U,
    /// The instance's geometry is a virtual-geometry asset, so cluster traversal owns it rather
    /// than an indexed draw. M7's; declared now so the bit number does not move later.
    kInstanceVirtualGeometry = 1U << 7U,
};

/// ONE INSTANCE, IN THE LAYOUT THE SHADER SEES.
///
/// 160 bytes, 16-byte aligned, no engine types inside it. That is not a style choice: requirement 1
/// above says a compute shader must be able to write these records, and a compute shader cannot
/// write a `cy::Transform` — it writes floats at offsets. The offsets are asserted at the bottom of
/// this file, and those assertions are the contract with the shader-side declaration. Change one
/// and the build tells you which; change the shader alone and nothing does, which is why the
/// assertions are here rather than in a test.
///
/// The transform is a row-major 4x3 (three `float4` rows) rather than a 4x4: the fourth row of an
/// affine object-to-world matrix is always (0, 0, 0, 1), and storing it would cost 16 bytes per
/// instance to hold a constant. `write_transform()` below is the only thing that fills it.
///
/// `previous_transform` and the two bounds are the specification's, verbatim: "The GPU scene SHALL
/// retain per-instance previous and current bounds, which shadow invalidation and motion vectors
/// both consume, so neither derives them independently."
struct alignas(16) GpuInstance {
    f32 transform[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    f32 previous_transform[12] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                  0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};

    /// World-space bounding sphere: centre and radius. A sphere rather than a box because it is
    /// four floats instead of six and because frustum rejection against a sphere is exact, while
    /// against a box it is conservative. The box is recoverable from the mesh's own bounds and the
    /// transform where a consumer needs one.
    f32 bounds_center[3] = {0.0F, 0.0F, 0.0F};
    f32 bounds_radius = 0.0F;
    f32 previous_center[3] = {0.0F, 0.0F, 0.0F};
    f32 previous_radius = 0.0F;

    /// Slot indices into the render server's own tables, not handles: a shader has no generation to
    /// check, and the CPU has already validated the handle by the time it writes a record.
    u32 mesh = 0;
    u32 material = 0;
    u32 lod_chain = 0;
    u32 layer_mask = 0;

    u32 flags = 0;

    /// `residency`'s **unified render importance**: "published per instance and consumed by every
    /// quality decision — geometry detail, texture page priority, shadow page resolution and
    /// refresh, animation rate, and illumination quality — so that subsystems do not maintain
    /// independent notions of what matters". One field, published once, by whoever publishes the
    /// instance.
    f32 importance = 0.0F;

    /// THE IDENTITY DETERMINISTIC SUBMISSION IS BUILT ON (design.md §6, sort.h).
    ///
    /// A value derived from what the instance *is* — an entity id for an extracted instance, a
    /// (producer, particle index) pair for a mesh particle — and never from the slot it landed in,
    /// which is free-list order. Two runs of the same frame produce the same stable ids and
    /// therefore the same sorted order, even if the slots differ. Split in two so the record has no
    /// 8-byte member and its layout is the same under every alignment rule a shader compiler has.
    u32 stable_id_low = 0;
    u32 stable_id_high = 0;

    [[nodiscard]] constexpr u64 stable_id() const noexcept {
        return (static_cast<u64>(stable_id_high) << 32U) | static_cast<u64>(stable_id_low);
    }
    constexpr void set_stable_id(u64 value) noexcept {
        stable_id_low = static_cast<u32>(value);
        stable_id_high = static_cast<u32>(value >> 32U);
    }
    [[nodiscard]] constexpr bool active() const noexcept { return (flags & kInstanceActive) != 0U; }
};

/// Write a placement into the record's 4x3, in the layout the shader reads.
void write_transform(GpuInstance& instance, const Transform& transform) noexcept;
/// The same, into `previous_transform`. Called with the previous frame's placement before the
/// current one is written, which is the order that keeps motion vectors a frame apart rather than
/// zero.
void write_previous_transform(GpuInstance& instance, const Transform& transform) noexcept;
/// Read the 4x3 back as a matrix. For a test and for CPU-side culling; nothing on a frame path
/// needs it.
[[nodiscard]] Mat4 read_transform(const GpuInstance& instance) noexcept;

/// Fill `bounds_center`/`bounds_radius` from a world-space box, and copy the current bounds into
/// the previous ones first. One function so that "previous bounds are the previous frame's" is a
/// property of the interface rather than of every caller remembering the order.
void write_bounds(GpuInstance& instance, const Aabb& world_bounds) noexcept;

// --- Producers ------------------------------------------------------------------------------

/// Which subsystem published a record. **Consumers never read this** — requirement 2 above is that
/// culling, LOD, sorting and drawing cannot tell — it exists so a statistics report and a debug
/// view can attribute slots, and so the store can refuse a release from the wrong owner.
enum class ProducerKind : u8 {
    /// `rendering-architecture`'s "extract stage, from ECS entities with renderable components".
    /// The only one M3 implements.
    Extract = 0,
    InstancedMesh,
    Vfx,
    Ui,
    Foliage,
    Terrain,
    Water,
    VirtualGeometry,
    /// A project's own. The interface is public, so a game may publish into the GPU scene with no
    /// engine change — which is the property that keeps the engine honest about it being one.
    Custom,
    Count,
};

[[nodiscard]] const char* producer_kind_name(ProducerKind kind) noexcept;

/// Who writes a reserved range's records.
///
/// THE DISTINCTION THAT MAKES GPU-SIDE PUBLICATION EXPRESSIBLE. A `Cpu` range is written through
/// `writable()` and uploaded from the dirty list; a `Gpu` range is written by a compute shader into
/// the device-side buffer and is NEVER uploaded — uploading it would overwrite the GPU's own work
/// with a stale CPU shadow every frame, which is precisely the CPU round trip `vfx-system` forbids.
enum class Residency : u8 {
    Cpu = 0,
    Gpu,
};

using ProducerId = u16;
inline constexpr ProducerId kInvalidProducer = 0xFFFFU;

/// A contiguous run of slots. Contiguous because a GPU producer addresses its output as
/// `base + thread_index`, and a scattered allocation would need an indirection table the shader
/// would have to read per particle.
struct InstanceRange {
    u32 first = 0;
    u32 count = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
    [[nodiscard]] constexpr u32 end() const noexcept { return first + count; }

    [[nodiscard]] friend constexpr bool operator==(InstanceRange a, InstanceRange b) noexcept {
        return a.first == b.first && a.count == b.count;
    }
};

struct ProducerInfo {
    const char* name = "";
    ProducerKind kind = ProducerKind::Custom;
    Residency residency = Residency::Cpu;
    /// Slots currently reserved by this producer, for the statistics report.
    u32 reserved_slots = 0;
    u32 reservations = 0;
};

struct GpuSceneStatistics {
    u32 capacity = 0;
    /// Slots in reserved ranges. Not the number of *active* records: a producer may reserve a
    /// thousand slots for an effect and have two hundred particles alive in them.
    u32 reserved_slots = 0;
    /// One past the highest slot ever reserved. What a GPU cull dispatch covers, and what the
    /// uploader uploads at most.
    u32 high_water = 0;
    u32 free_blocks = 0;
    /// The largest run a `reserve()` could satisfy right now. A producer that cannot get one big
    /// enough is the fragmentation symptom, and the number is here so it is reported rather than
    /// guessed at.
    u32 largest_free_run = 0;
    u32 producers = 0;
    /// Slots the CPU has marked dirty since the last `clear_dirty()`. What one frame's upload
    /// costs.
    u32 dirty_slots = 0;
};

// --- The store ------------------------------------------------------------------------------

/// The instance records, the slot allocator, and the dirty list. Not thread-safe: publication
/// happens from the frame's prepare stage, on one thread, and a mutex here would be paid by every
/// producer to serve none of them. A producer that publishes from workers reserves once on the
/// frame thread and writes its own disjoint slots from the workers, which needs no lock.
class GpuScene {
public:
    explicit GpuScene(Allocator& allocator) noexcept;

    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    /// Reserve `capacity` slots up front. The store never grows on its own: growing would move the
    /// records, and a `Residency::Gpu` producer holds a *device* buffer offset that a move would
    /// invalidate silently. `reserve()` fails when the store is full and the caller decides.
    [[nodiscard]] Status initialize(u32 capacity) noexcept;

    [[nodiscard]] Expected<ProducerId, Error> register_producer(const char* name, ProducerKind kind,
                                                                Residency residency) noexcept;
    [[nodiscard]] const ProducerInfo* producer(ProducerId id) const noexcept;
    [[nodiscard]] u32 producer_count() const noexcept {
        return static_cast<u32>(producers_.size());
    }

    /// Reserve a contiguous run for `producer`. The records are initialised to the inactive
    /// pattern, so a range is safe to cull over before its producer has written anything into it.
    [[nodiscard]] Expected<InstanceRange, Error> reserve(ProducerId producer, u32 count) noexcept;

    /// Return a range and clear its records. Refuses a range the producer does not own, and refuses
    /// one that was never reserved — both are the bug where two producers believe they hold the
    /// same slots, and the second symptom of that is a frame of someone else's particles.
    [[nodiscard]] Status release(ProducerId producer, InstanceRange range) noexcept;

    /// The records of a `Residency::Cpu` range, to be written and then marked dirty. Refused for a
    /// `Residency::Gpu` producer: writing those from the CPU is the round trip the interface exists
    /// to make impossible, and returning an empty span here is what makes it a compile-time-shaped
    /// mistake rather than a performance one nobody notices.
    [[nodiscard]] Expected<Span<GpuInstance>, Error> writable(ProducerId producer,
                                                              InstanceRange range) noexcept;

    /// Say that a range's records changed and must reach the device. Merged into the dirty list;
    /// adjacent and overlapping ranges coalesce, so a producer that marks each instance separately
    /// still costs one upload.
    [[nodiscard]] Status mark_dirty(InstanceRange range) noexcept;

    /// The ranges to upload this frame, ordered by first slot and non-overlapping.
    [[nodiscard]] Span<const InstanceRange> dirty_ranges() const noexcept { return dirty_.span(); }
    void clear_dirty() noexcept { dirty_.clear(); }

    /// Every record, including the holes. A consumer walks `[0, high_water())` and skips anything
    /// whose `kInstanceActive` bit is clear — which is exactly what a GPU cull does, so the CPU and
    /// GPU paths read the array the same way.
    [[nodiscard]] Span<const GpuInstance> instances() const noexcept { return records_.span(); }
    [[nodiscard]] const GpuInstance& at(u32 slot) const noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(records_.size()); }
    [[nodiscard]] u32 high_water() const noexcept { return high_water_; }

    [[nodiscard]] GpuSceneStatistics statistics() const noexcept;

    /// Return every slot and forget every producer. For a level unload and for a test; a frame
    /// never calls it.
    void reset() noexcept;

private:
    struct FreeBlock {
        u32 first = 0;
        u32 count = 0;
    };

    /// Which producer owns a slot run, so `release()` can refuse the wrong one.
    struct Reservation {
        u32 first = 0;
        u32 count = 0;
        ProducerId producer = kInvalidProducer;
    };

    [[nodiscard]] Status take_block(u32 count, u32& first) noexcept;
    [[nodiscard]] Status return_block(u32 first, u32 count) noexcept;
    [[nodiscard]] usize find_reservation(u32 first) const noexcept;
    void clear_records(u32 first, u32 count) noexcept;

    Array<GpuInstance> records_;
    Array<FreeBlock> free_;
    Array<Reservation> reservations_;
    Array<ProducerInfo> producers_;
    Array<InstanceRange> dirty_;
    u32 high_water_ = 0;
};

// --- The contract with the shader -----------------------------------------------------------
//
// A shader declares this struct too, and nothing at run time compares the two. These assertions are
// the comparison: they fail the build the moment a field moves, which is the only warning anybody
// gets. Keep them in step with the Slang declaration when one exists.

static_assert(sizeof(GpuInstance) == 160, "GpuInstance is the shader-side layout: 160 bytes");
static_assert(alignof(GpuInstance) == 16, "GpuInstance must be std430/std140-friendly");
static_assert(offsetof(GpuInstance, transform) == 0);
static_assert(offsetof(GpuInstance, previous_transform) == 48);
static_assert(offsetof(GpuInstance, bounds_center) == 96);
static_assert(offsetof(GpuInstance, bounds_radius) == 108);
static_assert(offsetof(GpuInstance, previous_center) == 112);
static_assert(offsetof(GpuInstance, previous_radius) == 124);
static_assert(offsetof(GpuInstance, mesh) == 128);
static_assert(offsetof(GpuInstance, material) == 132);
static_assert(offsetof(GpuInstance, lod_chain) == 136);
static_assert(offsetof(GpuInstance, layer_mask) == 140);
static_assert(offsetof(GpuInstance, flags) == 144);
static_assert(offsetof(GpuInstance, importance) == 148);
static_assert(offsetof(GpuInstance, stable_id_low) == 152);
static_assert(offsetof(GpuInstance, stable_id_high) == 156);
static_assert(std::is_trivially_copyable_v<GpuInstance>,
              "a record is memcpy'd into an upload buffer and written by a compute shader; it "
              "cannot have a non-trivial copy");

}  // namespace cy::render
