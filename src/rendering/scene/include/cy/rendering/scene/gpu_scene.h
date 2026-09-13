#pragma once
// The GPU scene: the shared GPU-side instance representation, and the interface producers publish
// into. Task 4.1.4, design.md §4.
//
// `rendering-architecture` — "GPU scene". Read the requirement before this file: it is short, and
// every decision below is traceable to a sentence in it.
//
// ================================================================================================
// THIS IS A PUBLICATION INTERFACE, NOT A MESH RENDERER'S INSTANCE ARRAY.
// ================================================================================================
//
// At M3 there is exactly one producer — the extract stage, publishing from ECS entities. From M7
// there are six, and the specification already names them: instanced mesh components, the VFX
// system's mesh particles, world-space and surface-space UI documents, foliage clusters, terrain
// and water as procedural sources, and virtual geometry's clusters. None of them goes through the
// ECS and none of them takes a CPU round trip.
//
// design.md §4 states the failure mode this file exists to avoid: designing the interface as
// "whatever the mesh renderer needs" and generalising later, at which point the second producer
// arrives with a requirement the interface cannot express and the interface becomes two
// interfaces. One producer is the cheapest moment to get the shape right, *provided* the shape is
// designed for the producers that are coming. So three properties are load-bearing here and none of
// them is needed by M3's single producer:
//
//   1. A producer RESERVES A CONTIGUOUS RANGE and then fills it. It does not append instances one
//      at a time. A compute shader cannot call `add_instance()`; it can write slot `base + tid`.
//      Reservation is therefore the only publication primitive, and CPU publication is the special
//      case where the fill happens to be a memcpy (`write_instances`) rather than a dispatch.
//
//   2. A range's contents may be GPU-AUTHORED. `declare_gpu_written()` says "this range is filled
//      by a dispatch this frame; the CPU mirror holds only the conservative bounds I am declaring".
//      That is the scenario "GPU-side publication" asks for, and it is a *declaration* rather than
//      an upload, so no readback exists to be optimised away later.
//
//   3. RETIREMENT IS PER PRODUCER, not per instance. "Producer removed ... without requiring a full
//      rebuild" is satisfied by returning the producer's ranges to the free list; nothing else in
//      the scene is touched and no other producer's slots move.
//
// WHAT IS DELIBERATELY THE SAME FOR EVERY PRODUCER. The record — `GpuInstance` — is one type. There
// is no `ParticleInstance` and no `FoliageInstance`, because "downstream culling, LOD, sorting, and
// drawing require no knowledge of an instance's origin" is only true if there is nothing downstream
// that could branch on the origin. `ProducerHandle` is stored for retirement and for diagnostics,
// and no consumer of the instance data is given it.
//
// ================================================================================================
// WHY SLOT INDICES ARE NOT AN ORDERING, AND WHERE THE ORDERING COMES FROM INSTEAD
// ================================================================================================
//
// A slot index is allocation order, which is publication order, which design.md §6 forbids draw
// order from depending on. That is not a defect in the allocator — it is why sort_key.h derives its
// key from content identity (material program, mesh, depth) and never from a slot index or a handle
// value. If you are tempted to sort by `InstanceRange::first` because it is already sorted, read
// sort_key.h's header first.
//
// ================================================================================================
// WHAT IS NOT HERE
// ================================================================================================
//
// No buffer, no descriptor, no upload. `GpuScene` is the CPU mirror and the allocation policy; the
// RHI-side buffer that mirrors it is the render graph's, and it reads `dirty_ranges()` to know what
// to transfer. Keeping the two apart is what lets every test in this module run with no device, and
// it is what makes the null backend's frame the same frame the Vulkan backend records.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/scene/handles.h>

namespace cy::rendering {

/// Where an instance came from. Diagnostics and retirement only — no draw path branches on it.
///
/// The list is `rendering-architecture`'s list of producers, written out at M3 although five of
/// them publish nothing until M7. A `ProducerKind` that is added later is added here; a producer
/// that does not fit one of these is `Custom`, which is what a project's own source uses.
enum class ProducerKind : u8 {
    /// The extract stage, from ECS entities with renderable components.
    Extract = 0,
    /// An instanced-mesh component, from its transform buffer.
    InstancedMesh,
    /// The VFX system's mesh particles (`vfx-system`).
    Vfx,
    /// World-space and surface-space UI documents (`ui-system`).
    Ui,
    /// Foliage instance clusters (`foliage`).
    Foliage,
    /// Terrain, as a procedural geometry source.
    Terrain,
    /// Water, as a procedural geometry source.
    Water,
    /// Virtual geometry cluster publication (`virtual-geometry`).
    VirtualGeometry,
    /// A project's or plugin's own source.
    Custom,
    Count,
};

const char* producer_kind_name(ProducerKind kind) noexcept;

/// Where a producer's instance data is written from.
///
/// This is the property that makes the interface usable by a compute shader, and it is declared at
/// registration rather than inferred per write so that the scene can refuse the confusion — a
/// GPU-side producer calling `write_instances()` is a bug worth naming, not a slow path.
enum class PublicationSite : u8 {
    /// The producer fills its ranges with `write_instances()`. The CPU mirror is authoritative.
    Cpu = 0,
    /// The producer fills its ranges from a compute dispatch. The CPU mirror holds only the
    /// conservative bounds the producer declared, and `write_instances()` is refused.
    Gpu,
};

/// Per-instance flags. A bit set rather than an enum: an instance is several of these at once.
enum class InstanceFlags : u32 {
    None = 0,
    /// Contributes to shadow maps.
    CastsShadow = 1U << 0U,
    /// Receives shadows. Distinct from casting: a shadow-casting proxy often does neither.
    ReceivesShadow = 1U << 1U,
    /// The transform changes between frames, so motion vectors and shadow invalidation must treat
    /// it as moving even when this frame's transform happens to match the last one's.
    Dynamic = 1U << 2U,
    /// Vertices come from a skinning compute pass's output buffer rather than from the mesh.
    Skinned = 1U << 3U,
    /// Rendered with both faces, with the normal flipped on the back face.
    TwoSided = 1U << 4U,
    /// The material alpha-tests, so the instance participates in the depth prepass with the test
    /// applied rather than being skipped.
    AlphaTested = 1U << 5U,
    /// Excluded from ray tracing acceleration structures.
    NoRayTracing = 1U << 6U,
    /// Written by a compute dispatch this frame. Set by `declare_gpu_written()`, never by a caller.
    GpuAuthored = 1U << 7U,
};

[[nodiscard]] constexpr InstanceFlags operator|(InstanceFlags a, InstanceFlags b) noexcept {
    return static_cast<InstanceFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr InstanceFlags operator&(InstanceFlags a, InstanceFlags b) noexcept {
    return static_cast<InstanceFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr InstanceFlags& operator|=(InstanceFlags& a, InstanceFlags b) noexcept {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr bool has_flag(InstanceFlags set, InstanceFlags flag) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0U;
}

/// How much this instance matters, on [0, 1], published once and consumed by everything.
///
/// `rendering-architecture`: "Render importance SHALL be published once per instance and consumed
/// by every quality decision — geometry detail, texture page priority, shadow page resolution and
/// refresh, animation rate, and illumination quality — so that subsystems do not maintain
/// independent notions of what matters."
///
/// It is a named type rather than a bare `f32` for exactly that reason. A subsystem that computes
/// its own importance has to declare a second field of a second type, which is visible in review;
/// a subsystem that multiplies this one by a per-subsystem weight is doing the sanctioned thing and
/// still reads the same source value.
struct RenderImportance {
    f32 value = 1.0f;

    [[nodiscard]] static constexpr RenderImportance clamped(f32 raw) noexcept {
        const f32 low = raw < 0.0f ? 0.0f : raw;
        return RenderImportance{low > 1.0f ? 1.0f : low};
    }

    friend constexpr bool operator==(RenderImportance, RenderImportance) noexcept = default;
};

/// An affine transform as the GPU reads it: three rows of four floats, 48 bytes.
///
/// A `Mat4` is 64 bytes and its fourth row is always (0, 0, 0, 1) for every transform an instance
/// can carry. Storing the row costs 16 bytes per transform and there are two transforms per
/// instance, so it is 32 bytes on every instance in the scene to store a constant. The conversion
/// is exact in both directions and is asserted so in the tests.
struct AffineTransform3x4 {
    /// Row-major: `m[row * 4 + column]`. Row-major rather than the engine's column-major `Mat4`
    /// because a 3x4 written column-major has its translation split across three vectors, and the
    /// shader-side unpack is then three loads instead of one.
    f32 m[12] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

    [[nodiscard]] static AffineTransform3x4 from_mat4(const Mat4& matrix) noexcept;
    [[nodiscard]] Mat4 to_mat4() const noexcept;
};

static_assert(sizeof(AffineTransform3x4) == 48);

/// One published instance. The whole of what a producer says about a renderable.
///
/// `rendering-architecture` fixes the minimum contents: "a transform and its previous-frame value,
/// bounds, a mesh reference, a material reference, an LOD chain reference, a layer mask, instance
/// flags, and a render importance value", plus "per-instance previous and current bounds, which
/// shadow invalidation and motion vectors both consume, so neither derives them independently".
///
/// `previous_transform` and `previous_bounds` are maintained by `GpuScene`, not by the producer: a
/// producer supplies this frame's values and the scene shifts the last frame's into place on the
/// first write of each frame. A producer that wrote them itself would have to know whether it had
/// already written this frame, which is bookkeeping the scene already has.
struct GpuInstance {
    AffineTransform3x4 transform;
    AffineTransform3x4 previous_transform;
    Aabb bounds = Aabb::empty();
    Aabb previous_bounds = Aabb::empty();

    MeshHandle mesh;
    MaterialHandle material;
    LodChainHandle lod_chain;

    /// Which view layers this instance appears in. A view's layer mask is ANDed with it.
    u32 layer_mask = 0xFFFFFFFFU;
    InstanceFlags flags = InstanceFlags::CastsShadow | InstanceFlags::ReceivesShadow;
    RenderImportance importance;

    /// An index into the producer's own per-instance payload — a tint, a particle's colour, a UI
    /// document's transform index. The scene neither reads it nor knows its stride; it is carried
    /// so that a shader can reach the producer's data from the one record every producer shares.
    u32 user_data = 0;

    /// True while the slot holds a published instance. A released slot keeps its bytes so that a
    /// dirty range covering it is still a valid transfer; this is what says the bytes are stale.
    bool live = false;
};

/// A contiguous run of instance slots. The unit of reservation, of release, and of transfer.
struct InstanceRange {
    u32 first = 0;
    u32 count = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
    [[nodiscard]] constexpr u32 end() const noexcept { return first + count; }

    friend constexpr bool operator==(InstanceRange, InstanceRange) noexcept = default;
};

/// What a producer declared at registration, and what it currently holds.
struct ProducerInfo {
    ProducerKind kind = ProducerKind::Custom;
    PublicationSite site = PublicationSite::Cpu;
    /// A literal, for diagnostics. Never freed, never copied.
    const char* name = "";
    u32 reserved_slots = 0;
    u32 range_count = 0;
};

/// The shared GPU-side instance representation.
///
/// Not thread safe. Publication happens on one thread at a defined point in the frame — the Prepare
/// stage — and a lock here would be a lock taken once per producer per frame to protect against a
/// concurrency the frame structure does not have. If parallel publication is wanted later, it is a
/// per-producer arena of pre-reserved ranges, which this interface already expresses.
class GpuScene {
public:
    explicit GpuScene(Allocator& allocator) noexcept;

    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    // --- Producers -------------------------------------------------------------------------------

    /// Register a publisher. `name` must outlive the scene — a literal, in practice.
    [[nodiscard]] Expected<ProducerHandle, Error> register_producer(ProducerKind kind,
                                                                    const char* name,
                                                                    PublicationSite site) noexcept;

    /// Release every range the producer holds and forget it. The scenario "Producer removed": no
    /// other producer's slots move and nothing is rebuilt.
    [[nodiscard]] Status retire_producer(ProducerHandle producer) noexcept;

    [[nodiscard]] Expected<ProducerInfo, Error> producer_info(
        ProducerHandle producer) const noexcept;

    // --- Reservation -----------------------------------------------------------------------------

    /// Reserve `count` contiguous slots for `producer`. The only way to obtain instance storage.
    [[nodiscard]] Expected<InstanceRange, Error> reserve(ProducerHandle producer,
                                                         u32 count) noexcept;

    /// Return a range this producer reserved. The slots are marked dead and coalesced into the free
    /// list; their bytes are left alone so a transfer covering them stays valid.
    [[nodiscard]] Status release(ProducerHandle producer, InstanceRange range) noexcept;

    // --- Publication -----------------------------------------------------------------------------

    /// Fill a reserved range from the CPU. Refused for a `PublicationSite::Gpu` producer.
    ///
    /// Each instance's previous transform and previous bounds are supplied by the scene: on the
    /// first write of a frame the current values shift into the previous ones, and a second write
    /// in the same frame overwrites the current values without shifting again.
    [[nodiscard]] Status write_instances(ProducerHandle producer, InstanceRange range,
                                         Span<const GpuInstance> instances) noexcept;

    /// Declare that a range is filled by a dispatch this frame.
    ///
    /// `conservative_bounds` is what CPU-side culling and shadow invalidation use for the whole
    /// range until GPU-side culling consumes the range directly. It is required rather than
    /// optional: a GPU producer that cannot bound its own output cannot be culled at all, and
    /// discovering that at M7 with six producers is worse than stating it now.
    [[nodiscard]] Status declare_gpu_written(ProducerHandle producer, InstanceRange range,
                                             const Aabb& conservative_bounds) noexcept;

    // --- The frame boundary ----------------------------------------------------------------------

    /// Open a frame. Nothing is copied: the shift from current to previous happens on the first
    /// write to each instance, so an instance nobody wrote keeps `previous == current` and produces
    /// no motion, which is the correct answer for a static instance and costs nothing.
    void begin_frame() noexcept;

    [[nodiscard]] u64 frame_index() const noexcept { return frame_index_; }

    // --- Reading ---------------------------------------------------------------------------------

    [[nodiscard]] Span<const GpuInstance> instances() const noexcept { return instances_.span(); }
    [[nodiscard]] const GpuInstance* instance(u32 slot) const noexcept;

    /// The slot ranges written since `clear_dirty()`, coalesced. What the transfer that mirrors
    /// this scene into a GPU buffer reads, and the reason publication is range-shaped.
    [[nodiscard]] Span<const InstanceRange> dirty_ranges() const noexcept { return dirty_.span(); }
    void clear_dirty() noexcept { dirty_.clear(); }

    /// Slots that have ever been reserved — the high-water mark, which is the buffer size a backend
    /// must allocate. Not the live count.
    [[nodiscard]] u32 slot_capacity() const noexcept { return static_cast<u32>(instances_.size()); }
    [[nodiscard]] u32 live_instances() const noexcept { return live_; }
    [[nodiscard]] u32 free_slots() const noexcept;

private:
    struct Producer {
        ProducerKind kind = ProducerKind::Custom;
        PublicationSite site = PublicationSite::Cpu;
        const char* name = "";
        u32 reserved_slots = 0;
        bool live = false;
    };

    /// Which frame an instance was last written in, so the current-to-previous shift happens once
    /// per frame per instance. Parallel to `instances_` rather than a member of `GpuInstance`,
    /// because it is the scene's bookkeeping and not part of what a producer publishes or what a
    /// backend uploads.
    struct SlotState {
        u64 written_frame = 0;
        u32 owner = 0;  ///< The owning producer's slot index, or `kNoOwner`.
    };

    static constexpr u32 kNoOwner = 0xFFFFFFFFU;

    [[nodiscard]] Producer* find_producer(ProducerHandle producer) noexcept;
    [[nodiscard]] const Producer* find_producer(ProducerHandle producer) const noexcept;
    /// Validate that `range` is inside the scene and wholly owned by `producer`.
    [[nodiscard]] Status check_owned(ProducerHandle producer, InstanceRange range) const noexcept;
    [[nodiscard]] Status grow_by(u32 count) noexcept;
    void mark_dirty(InstanceRange range) noexcept;
    void free_range(InstanceRange range) noexcept;
    /// First fit over the free list, splitting the block it lands in. Deterministic given the same
    /// sequence of calls, which is what a golden-image run replays.
    [[nodiscard]] bool take_free(u32 count, InstanceRange& out) noexcept;

    Allocator* allocator_;
    Array<GpuInstance> instances_;
    Array<SlotState> slots_;
    Array<InstanceRange> free_;
    Array<InstanceRange> dirty_;
    Array<Producer> producers_;
    GenerationTable producer_generations_;
    u64 frame_index_ = 0;
    u32 live_ = 0;
};

}  // namespace cy::rendering
