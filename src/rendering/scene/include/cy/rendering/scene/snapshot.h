#pragma once
// The simulation-to-render snapshot, taken at M2's commit boundary. Task 4.1.2.
//
// `rendering-architecture` — "Simulation-to-render snapshot": rendering consumes an immutable
// snapshot published at a defined point each frame, not live ECS storage; the snapshot carries
// visible instance data, light state, camera state and environment state; extraction is
// incremental, using change detection; and the transforms it writes are interpolated with the
// frame's alpha.
//
// ================================================================================================
// THE DEFINED POINT IS NOT A NEW ONE
// ================================================================================================
//
// M2 built exactly one moment at which state becomes authoritative, and `<cy/core/determinism/
// commit.h>` is explicit that every consumer of authoritative state keys off it rather than
// defining its own: "a consumer does not *ask* when the tick committed, it is *called* with a
// `CommitRecord`". `SnapshotPublisher` is therefore a `determinism::CommitObserver` and nothing
// else. It cannot take a snapshot at a moment of its own choosing, because it has no way to find
// one — which is the property that keeps the renderer from becoming the second definition of "now".
//
// ================================================================================================
// WHAT CROSSES THE FIREWALL, AND IN WHICH DIRECTION
// ================================================================================================
//
// A snapshot is presentation state: it exists so that something can be drawn, its contents depend
// on when the frame happened to be rendered, and an authoritative system that read one would be
// reading a value it must not depend on. The two fields where that is a live risk — the
// interpolation alpha and the interpolated transforms derived from it — are wrapped in
// `determinism::Presentation<>`, so an authoritative witness cannot name them at all.
//
// This is task 1.3's adoption for the renderer, and the shape M2's `scene::InterpolatedTransform`
// set: authority writes down, presentation reads up, nothing reads back. Where a rendered outcome
// genuinely has to influence gameplay — a hit test against rendered geometry — the route is
// `determinism::record_external()`, which is spellable, named and recorded.
//
// ================================================================================================
// INCREMENTAL, AND WHY THE SNAPSHOT CARRIES CHANGES RATHER THAN A WORLD
// ================================================================================================
//
// "WHEN 100 000 static instances exist and 50 move, THEN only the 50 changed instances SHALL be
// re-extracted." A snapshot that carried every visible instance would satisfy the letter of
// "immutable" and none of the point: the cost of extraction would be the size of the world rather
// than the size of the change. So a snapshot is a *delta* — instances published, instances removed
// — applied to the GPU scene, which is the thing that holds the whole world. The GPU scene's slot
// allocation is what makes the delta applicable: an instance keeps its slot across frames, so an
// update is a write to a slot rather than a rebuild.
//
// WHAT IS NOT HERE. The extraction itself. Reading ECS components is `src/runtime/`'s at layer 5;
// this module is layer 2 and, by `src/servers/README.md`'s rule, must never dereference an entity.
// What is here is the type the extract stage fills and the renderer reads, which is the seam
// between them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/determinism/commit.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/scene/gpu_scene.h>

namespace cy::rendering {

/// One instance the extract stage published this frame.
///
/// `identity` is the stable identity the sort key is built from — an entity id, in the extract
/// stage's case. It is carried separately from the GPU scene slot because the slot is allocation
/// order and the identity is content; see sort_key.h.
struct InstanceUpdate {
    u64 identity = 0;
    GpuInstance instance;
};

/// A light, in the physical units `rendering-lighting-and-shadows` requires.
///
/// Seed-level at M3 and deliberately small: the snapshot has to carry lights for the frame to be
/// lit, and inventing a rich light model here — before the milestone that specifies shadows,
/// cascades and atlas allocation — would be inventing the parts that milestone has to change.
struct LightState {
    u64 identity = 0;
    Vec3 position;
    Vec3 direction{0.0f, 0.0f, -1.0f};
    /// Linear RGB, unitless. Multiplied by `intensity` to give the emitted quantity.
    Vec3 color{1.0f, 1.0f, 1.0f};
    /// Lumens for a point or spot light, lux for a directional one. Named in the field comment
    /// rather than in a wiki, because a light whose unit is ambiguous is a light that gets
    /// multiplied by an arbitrary constant somewhere downstream.
    f32 intensity = 0.0f;
    f32 range = 0.0f;
    /// Cone half-angles in radians, for a spot light. Zero for the other kinds.
    f32 inner_cone = 0.0f;
    f32 outer_cone = 0.0f;
    u32 layer_mask = 0xFFFFFFFFU;
    bool casts_shadow = true;
    /// 0 directional, 1 point, 2 spot. An enum arrives with the lighting milestone; a `u8` here
    /// keeps the snapshot from pretending to a model it does not yet have.
    u8 kind = 0;
};

/// An evaluated camera, as `rendering-architecture` requires views be produced from.
///
/// The camera supplies pose and *projection semantics*; the renderer builds the matrix. That is why
/// this carries a field of view and planes rather than a `Mat4` — a camera that shipped a matrix
/// would have decided the depth convention, the handedness and the Y flip on the renderer's behalf.
struct CameraState {
    u64 identity = 0;
    /// The interpolated pose. Presentation: it is a blend between two ticks, and its value depends
    /// on when the frame was drawn.
    determinism::Presentation<Vec3> position;
    determinism::Presentation<Quat> rotation;
    f32 vertical_fov_radians = 1.0f;
    f32 near_plane = 0.1f;
    /// Zero means the infinite far plane, which reversed Z makes the reasonable default.
    f32 far_plane = 0.0f;
    f32 aspect = 1.0f;
    /// How much of the frame budget this camera's views may take, relative to the others.
    f32 importance = 1.0f;
    u32 layer_mask = 0xFFFFFFFFU;
    /// Stable across frames, so temporal history and a golden image both find the same view again.
    u64 history_identity = 0;
};

/// The environment: background, ambient source, and fog. Per `rendering-architecture`'s
/// "Environment and post-process configuration", at the depth M3's frame consumes.
struct EnvironmentState {
    Vec3 background_color;
    Vec3 ambient_color;
    f32 ambient_intensity = 0.0f;
    /// Spherical-harmonic irradiance and a pre-filtered environment map arrive through
    /// `rendering-material`'s IBL; this handle names which one, or is null.
    TextureHandle environment_map;
    f32 fog_density = 0.0f;
    Vec3 fog_color;
};

/// What the renderer reads. Immutable: every accessor is const and the arrays are spans into
/// storage the publisher owns and does not touch while this snapshot is the readable one.
class RenderSnapshot {
public:
    RenderSnapshot() = default;

    [[nodiscard]] u64 state_version() const noexcept { return state_version_; }
    [[nodiscard]] determinism::SimulationPoint point() const noexcept { return point_; }

    /// The interpolation alpha the clock computed, between the last two ticks. Presentation state:
    /// an authoritative witness has no overload that reads it.
    [[nodiscard]] const determinism::Presentation<f32>& interpolation_alpha() const noexcept {
        return alpha_;
    }

    [[nodiscard]] Span<const InstanceUpdate> published() const noexcept { return published_; }
    [[nodiscard]] Span<const u64> removed() const noexcept { return removed_; }
    [[nodiscard]] Span<const LightState> lights() const noexcept { return lights_; }
    [[nodiscard]] Span<const CameraState> cameras() const noexcept { return cameras_; }
    [[nodiscard]] const EnvironmentState& environment() const noexcept { return environment_; }

    /// True once the publisher has filled this. A renderer handed an unpublished snapshot draws
    /// nothing rather than drawing the previous frame twice.
    [[nodiscard]] bool valid() const noexcept { return state_version_ != 0; }

private:
    friend class SnapshotPublisher;

    u64 state_version_ = 0;
    determinism::SimulationPoint point_;
    determinism::Presentation<f32> alpha_;
    Span<const InstanceUpdate> published_;
    Span<const u64> removed_;
    Span<const LightState> lights_;
    Span<const CameraState> cameras_;
    EnvironmentState environment_;
};

/// What the extract stage writes into. One per publisher, reused every frame.
class SnapshotBuilder {
public:
    explicit SnapshotBuilder(Allocator& allocator) noexcept;

    SnapshotBuilder(const SnapshotBuilder&) = delete;
    SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;

    /// Drop last frame's contents. Capacity is kept: extraction is a per-frame allocation-free path
    /// after the first few frames, which is what "no allocation per primitive" costs elsewhere too.
    void reset() noexcept;

    [[nodiscard]] Status publish(u64 identity, const GpuInstance& instance) noexcept;
    [[nodiscard]] Status remove(u64 identity) noexcept;
    [[nodiscard]] Status add_light(const LightState& light) noexcept;
    [[nodiscard]] Status add_camera(const CameraState& camera) noexcept;
    void set_environment(const EnvironmentState& environment) noexcept;

    /// The interpolation alpha for this frame. Takes a presentation witness, because writing it is
    /// a presentation act — the runtime hands the alpha to the render half rather than the render
    /// half fetching a clock.
    void set_interpolation_alpha(determinism::PresentationContext witness, f32 alpha) noexcept;

    [[nodiscard]] usize published_count() const noexcept { return published_.size(); }
    [[nodiscard]] usize removed_count() const noexcept { return removed_.size(); }

private:
    friend class SnapshotPublisher;

    Array<InstanceUpdate> published_;
    Array<u64> removed_;
    Array<LightState> lights_;
    Array<CameraState> cameras_;
    EnvironmentState environment_;
    determinism::Presentation<f32> alpha_;
};

/// The commit-boundary observer that makes a built snapshot readable.
///
/// Double buffered: the extract stage fills the back builder while the renderer reads the front
/// snapshot, and `on_commit()` swaps them. That is the whole of the "Render reads a consistent
/// world" scenario — the renderer's spans never point into storage anything is writing, because the
/// only writer is working in the other buffer.
class SnapshotPublisher final : public determinism::CommitObserver {
public:
    explicit SnapshotPublisher(Allocator& allocator) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "render-snapshot"; }

    /// Swap the buffers and stamp the new readable snapshot with the record.
    [[nodiscard]] Status on_commit(const determinism::CommitRecord& record) noexcept override;

    /// The builder the extract stage writes into. Never the one `readable()` describes.
    [[nodiscard]] SnapshotBuilder& extraction() noexcept { return *back_; }

    /// The immutable snapshot the renderer reads.
    [[nodiscard]] const RenderSnapshot& readable() const noexcept { return readable_; }

    /// How many commits have been published. What a test asserts on to say the seam is wired.
    [[nodiscard]] u64 published_frames() const noexcept { return published_frames_; }

private:
    SnapshotBuilder buffers_[2];
    SnapshotBuilder* front_;
    SnapshotBuilder* back_;
    RenderSnapshot readable_;
    u64 published_frames_ = 0;
};

}  // namespace cy::rendering
