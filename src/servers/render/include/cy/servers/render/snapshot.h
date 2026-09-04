#pragma once
// The simulation-to-render snapshot. Task 4.1.2.
//
// `rendering-architecture` — "Simulation-to-render snapshot": "Rendering SHALL consume an immutable
// render snapshot published at a defined point each frame, not live ECS storage", containing
// "visible instance data (transforms, bounds, material and mesh handles, per-instance parameters),
// light state, camera state, and environment state", with extraction that is INCREMENTAL — "only
// instances whose relevant components changed since the last extraction SHALL be re-extracted".
//
// --- THE DEFINED POINT IS M2'S COMMIT BOUNDARY, AND THAT IS NOT A DETAIL
// --------------------------
//
// `simulation-and-determinism` already has one: `determinism::CommitBoundary` is the single moment
// per tick at which state becomes authoritative, and its whole design argument is that "every
// consumer of authoritative state keys off it rather than defining its own moment". A renderer that
// sampled the world at some other point would be the second consumer with its own moment, which is
// the thing that boundary exists to prevent.
//
// So extraction is a `determinism::CommitObserver` (`cy::rendering::SnapshotExtractor`, in
// src/rendering/scene/, which is where the ECS lives). This file is the snapshot itself: what it
// contains, and the double-buffered exchange that lets the render thread read one while the
// simulation fills the next.
//
// --- WHY A TICK BOUNDARY CARRIES AN INTERPOLATION ALPHA IT DOES NOT KNOW -------------------------
//
// The runtime runs N fixed ticks and then one variable-rate render (`src/runtime/simulation.h`), so
// a frame usually falls BETWEEN two commits. The requirement is that "WHEN rendering falls between
// simulation ticks THEN extraction SHALL write interpolated transforms using the frame's
// interpolation alpha".
//
// A snapshot therefore carries BOTH placements — `previous_transform` and `transform`, captured at
// two commits — and the alpha is applied at frame time by `resolve_transform()`. Baking the blend
// into the snapshot at commit time would be wrong twice over: the alpha is not known at the commit
// (the frame has not happened yet), and one snapshot is commonly rendered by several views at one
// alpha, so blending once per instance rather than once per instance per view is the cheaper of the
// two anyway.
//
// --- WHY THE SNAPSHOT IS A DIFF AND NOT A WORLD --------------------------------------------------
//
// "WHEN 100 000 static instances exist and 50 move THEN only the 50 changed instances SHALL be
// re-extracted." A snapshot that carried every instance would make that sentence untrue by
// construction, however fast the copy was. So it carries CHANGES: instances added or updated,
// instances removed, and the frame-wide state (cameras, lights, environment) that is small enough
// to carry whole. The GPU scene is the accumulation of them, and it is what a view actually reads.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/model.h>

namespace cy::render {

/// One instance's extracted state.
///
/// The transforms are `determinism::Presentation`-classified at the point they are *consumed*
/// rather than here: this struct is written by the extractor, which is presentation code, and read
/// by the renderer, which is presentation code. Wrapping every field would put a witness in every
/// line of the uploader and buy nothing, because no authoritative system can reach a snapshot at
/// all — it is handed to the render half and nothing else has a pointer to it. The classified
/// fields are on the ECS components the extractor reads (`cy::rendering::MeshRenderer`), which is
/// where an authoritative system could otherwise have reached them.
struct InstanceSnapshot {
    /// The instance's stable identity — an entity id for an extracted instance. Everything in the
    /// snapshot is keyed by it, so a snapshot is meaningful without knowing which slot anything
    /// landed in. See sort.h.
    u64 stable_id = 0;
    MeshHandle mesh;
    MaterialHandle material;
    /// The two placements interpolation blends between: the tick before this one, and this one.
    Transform previous_transform;
    Transform transform;
    Aabb local_bounds = Aabb::empty();
    LayerMask layer_mask = kDefaultLayer;
    f32 importance = 1.0F;
    f32 lod_bias = 0.0F;
    /// `InstanceFlagBits` (gpu_scene.h): the boolean set, as the bits the GPU record already
    /// defines, rather than as four `bool`s that would then have to be translated into them.
    /// `apply_snapshot()` reads `kInstanceVisible`, `kInstanceCastsShadow`,
    /// `kInstanceReceivesShadow` and `kInstanceTwoSided`; the rest are the producer's and reach the
    /// record untouched.
    ///
    /// ZERO IS "NOT VISIBLE, CASTS NOTHING", which is why an extractor sets the bits explicitly
    /// rather than relying on a default. A snapshot whose flags nobody filled in publishes an
    /// invisible instance, and that is the failure a reader can see rather than one where an
    /// invisible object draws.
    u32 flags = 0;
    /// Set by `Node::teleport()`'s equivalent: this instance jumped, so blending between the two
    /// placements would draw it smearing across the gap. `resolve_transform()` returns the current
    /// placement instead.
    bool teleported = false;
};

/// A camera as the snapshot carries it. `rendering-architecture` requires views to be produced from
/// *evaluated* cameras, so what crosses the boundary is the evaluated pose and the semantic
/// projection — never a camera rig, and never a matrix.
struct CameraSnapshot {
    u64 stable_id = 0;
    Transform previous_transform;
    Transform transform;
    Projection projection;
    ViewportRect viewport;
    LayerMask layer_mask = kAllLayers;
    f32 importance = 1.0F;
    u64 history_id = 0;
    bool teleported = false;
};

struct LightSnapshot {
    LightDescription desc;
    Transform previous_transform;
    bool teleported = false;
};

/// One published snapshot.
///
/// Immutable once published: `SnapshotBuffer::publish()` is the only thing that fills one, and a
/// reader holds a `const` reference. The immutability is what makes "WHEN the render thread builds
/// a frame THEN it SHALL see a coherent snapshot even while simulation advances concurrently" true
/// — the simulation is writing the *other* buffer.
struct RenderSnapshot {
    explicit RenderSnapshot(Allocator& allocator) noexcept
        : changed(allocator), removed(allocator), cameras(allocator), lights(allocator) {}

    RenderSnapshot(const RenderSnapshot&) = delete;
    RenderSnapshot& operator=(const RenderSnapshot&) = delete;

    /// Which committed tick this is a picture of. `determinism::CommitRecord::state_version`, so a
    /// divergence report and a rendered frame can be lined up against each other.
    u64 state_version = 0;
    determinism::SimulationPoint point;

    /// Instances added or changed since the previous snapshot. The incremental half.
    Array<InstanceSnapshot> changed;
    /// Stable ids of instances that no longer exist.
    Array<u64> removed;
    /// Carried whole rather than incrementally: a frame has a handful of cameras and hundreds of
    /// lights, and the bookkeeping to diff them would cost more than the copy.
    Array<CameraSnapshot> cameras;
    Array<LightSnapshot> lights;
    EnvironmentSettings environment;

    /// How many instances the extractor examined to produce `changed`. Reported so that "only the
    /// 50 changed instances were re-extracted" is a measurement rather than a claim: a test asserts
    /// on `changed.size()` against `examined`.
    u32 examined = 0;

    void clear() noexcept {
        changed.clear();
        removed.clear();
        cameras.clear();
        lights.clear();
        examined = 0;
    }
};

/// The placement to render, given the frame's interpolation alpha.
///
/// `alpha` is the runtime's, in [0, 1], where 0 is the previous tick and 1 is the current one. A
/// teleported instance ignores it — that is the "teleport flag SHALL suppress interpolation for
/// that frame" rule, and it is applied here so that every consumer gets it right by calling one
/// function.
[[nodiscard]] Transform resolve_transform(const Transform& previous, const Transform& current,
                                          f32 alpha, bool teleported) noexcept;

/// Two snapshots and the exchange between them.
///
/// SINGLE PRODUCER, SINGLE CONSUMER, AND NO LOCK ON THE READ. The simulation fills the back buffer
/// and publishes it with a release store; the renderer acquires the front buffer and reads it. The
/// renderer holds its snapshot for the whole frame, so a publish that happens mid-frame lands in
/// the buffer the renderer is not reading — which is the entire synchronisation argument, and it is
/// why there are exactly two buffers rather than a queue.
class SnapshotBuffer {
public:
    explicit SnapshotBuffer(Allocator& allocator) noexcept;

    SnapshotBuffer(const SnapshotBuffer&) = delete;
    SnapshotBuffer& operator=(const SnapshotBuffer&) = delete;

    /// The buffer the extractor writes into. Cleared for you, HERE rather than at the previous
    /// `publish()` — see the implementation for why that difference is what keeps a reader's
    /// snapshot alive across a mid-frame publish.
    [[nodiscard]] RenderSnapshot& writable() noexcept;

    /// Make the written buffer the readable one. Returns the version it was published under.
    u64 publish() noexcept;

    /// The most recently published snapshot, or null before the first publish. A caller that gets
    /// null renders nothing, which is the correct first frame rather than an error.
    [[nodiscard]] const RenderSnapshot* readable() const noexcept;

    /// How many publishes have happened. The renderer compares it against the one it last consumed
    /// to answer "is this a new snapshot", without inspecting the contents.
    [[nodiscard]] u64 published_count() const noexcept { return published_; }

private:
    /// Initialised in the constructor because both elements need the allocator; the tidy check's
    /// suggested default member initializer would name a constructor parameter, which a default
    /// member initializer cannot see.
    // NOLINTNEXTLINE(modernize-use-default-member-init)
    RenderSnapshot buffers_[2];
    /// Index of the buffer being written. The other one is readable.
    u32 writing_ = 0;
    u64 published_ = 0;
    bool has_published_ = false;
};

}  // namespace cy::render
