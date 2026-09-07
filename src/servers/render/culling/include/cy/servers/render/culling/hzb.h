#pragma once
// The hierarchical depth buffer, and the two-pass scheme built on it. M6 task 8.5.
//
// `rendering-culling-and-lod` — "Occlusion culling": "The engine SHALL support occlusion culling
// using a **hierarchical depth buffer** built from the previous frame's depth, reprojected into the
// current view. Instances SHALL be tested by projecting their bounds and comparing against the
// appropriate HZB mip. A **visibility hysteresis** SHALL keep recently visible instances visible
// for a configurable number of frames to prevent flicker from reprojection error."
//
// ================================================================================================
// WHAT LIVES HERE AND WHAT CANNOT
// ================================================================================================
//
// The pyramid is BUILT on the GPU — a chain of compute reductions over the depth target — and this
// module is layer 2 and may not name a texture. So what is here is the pyramid's SHAPE and its
// TEST: how many levels a resolution has, which level a projected rectangle must be read at, how a
// sphere projects to that rectangle, and what the comparison is. The device-side reduction fills
// the levels this class holds; a headless test fills them itself, which is what makes the whole
// occlusion path assertable with no GPU.
//
// The pyramid stores the FURTHEST depth of each footprint, not the nearest, and under the engine's
// reversed-Z convention (`core-math`) that is the SMALLEST value. An instance is occluded when its
// NEAREST depth is further than the furthest thing already drawn over its whole footprint. Getting
// this inequality backwards produces a renderer that culls everything that is visible and draws
// everything that is not, which looks like a broken pyramid rather than a flipped comparison — so
// it is stated here, tested in `test_hzb.cpp`, and asserted by `kReversedZ`.
//
// ================================================================================================
// FALSE OCCLUSION IS A CORRECTNESS BUG, NOT A QUALITY ONE
// ================================================================================================
//
// "False occlusion SHALL be impossible for correctness-critical cases: reprojection SHALL be
// conservative, and newly disoccluded regions SHALL be treated as visible." Three mechanisms here,
// and each is a method rather than a comment:
//
//   * `Hzb::invalidate()` — a camera cut discards the previous frame's depth entirely, and the
//     tester then answers "not occluded" for everything. "WHEN the camera teleports THEN the
//     previous frame's depth SHALL be discarded and no occlusion culling SHALL be applied for that
//     frame."
//   * `VisibilityHistory` — an instance that was visible within the last `frames` frames is kept
//     visible, which is the hysteresis the requirement names.
//   * `TwoPassCull` — draw what was visible last frame, rebuild the pyramid from THAT depth, then
//     test the rest against it. Geometry that became visible because an occluder moved is drawn in
//     the same frame rather than a frame late.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/culling/gpu_cull.h>

namespace cy::render::culling {

/// The engine's depth convention, stated as a constant so the comparison below cannot be read as
/// arbitrary. Under reversed Z, 1 is the near plane and 0 the far plane, so "further away" is
/// "smaller".
inline constexpr bool kReversedZ = true;

/// How many mip levels a pyramid of this size has: enough that the coarsest is a single texel.
[[nodiscard]] u32 hzb_level_count(u32 width, u32 height) noexcept;

/// A rectangle of the depth buffer, in texels, that a sphere's bounds project onto.
struct ScreenRect {
    f32 min_x = 0.0F;
    f32 min_y = 0.0F;
    f32 max_x = 0.0F;
    f32 max_y = 0.0F;
    /// The nearest depth of the sphere, in the same [0, 1] reversed-Z space the buffer holds. 1 at
    /// the near plane.
    f32 nearest_depth = 0.0F;
    /// False when the sphere is behind the eye, straddles the near plane, or projects to nothing.
    /// Such a sphere is never occluded: it is either in the camera's face or not on screen, and the
    /// frustum test has already had its say.
    bool valid = false;
};

/// Project a world-space bounding sphere onto the depth buffer.
///
/// Conservative by construction: the rectangle is the projection of the sphere's bounding box in
/// view space, which contains the sphere's silhouette, and `nearest_depth` is taken at the sphere's
/// closest point. Both errors are in the direction of keeping an instance that could have been
/// dropped.
[[nodiscard]] ScreenRect project_sphere(Vec3 centre, f32 radius, const Mat4& view_projection,
                                        u32 width, u32 height) noexcept;

/// Which level to read a rectangle at: the one whose texels are large enough that the rectangle
/// spans at most two of them, so the test is four taps at most.
[[nodiscard]] u32 hzb_level_for(const ScreenRect& rect, u32 level_count) noexcept;

/// The pyramid: one array of depths per level, coarsest last.
///
/// Filled by the device's reduction chain in a real frame and by a test in a test. The class holds
/// no texture and no device; `level()` hands out the storage a copy writes into.
class Hzb {
public:
    explicit Hzb(Allocator& allocator) noexcept;
    ~Hzb();

    Hzb(const Hzb&) = delete;
    Hzb& operator=(const Hzb&) = delete;

    /// Size the pyramid. Existing contents are discarded and the pyramid is left INVALID, because a
    /// resize is a resolution change and the previous frame's depth means nothing at a new one.
    [[nodiscard]] Status resize(u32 width, u32 height) noexcept;

    /// Discard the previous frame's depth. Every test then answers "not occluded" until a level has
    /// been written and `mark_valid()` called. What a camera cut does.
    void invalidate() noexcept { valid_ = false; }
    void mark_valid() noexcept { valid_ = true; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] u32 level_count() const noexcept { return level_count_; }
    [[nodiscard]] u32 level_width(u32 level) const noexcept;
    [[nodiscard]] u32 level_height(u32 level) const noexcept;

    /// One level's depths, row major. Writable so the reduction — or a test — fills it.
    [[nodiscard]] Span<f32> level(u32 index) noexcept;
    [[nodiscard]] Span<const f32> level(u32 index) const noexcept;

    /// Build every level above 0 from level 0, keeping the FURTHEST depth of each 2x2 footprint.
    ///
    /// This is the reduction the device performs, here so a test does not have to reimplement it
    /// and so a headless build has a working pyramid. An odd dimension takes the extra row and
    /// column into the same parent texel, which is what keeps the pyramid conservative at the
    /// edges.
    void reduce() noexcept;

    /// Whether a projected rectangle is certainly behind what the pyramid holds.
    ///
    /// False whenever the pyramid is invalid, the rectangle is invalid, or the rectangle leaves the
    /// buffer — every one of those is a case where the answer is not known, and "not known" must
    /// read as "draw it".
    [[nodiscard]] bool occludes(const ScreenRect& rect) const noexcept;

private:
    void release_levels() noexcept;

    Allocator* allocator_ = nullptr;
    Array<Array<f32>*> levels_;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 level_count_ = 0;
    bool valid_ = false;
};

/// `Hzb` plus a view matrix, as the interface `cpu_reference_cull` takes.
class HzbOcclusionTester final : public OcclusionTester {
public:
    HzbOcclusionTester(const Hzb& pyramid, const Mat4& view_projection) noexcept;

    [[nodiscard]] bool occluded(Vec3 centre, f32 radius) const noexcept override;

private:
    const Hzb* pyramid_ = nullptr;
    Mat4 view_projection_;
};

/// The visibility hysteresis: an instance seen within the last `frames` frames stays visible.
///
/// State per slot rather than per instance object, so it survives an instance moving between
/// producers and costs one byte each. `frames` of 0 disables it, which is what a deterministic test
/// wants and what a golden-image comparison needs.
class VisibilityHistory {
public:
    explicit VisibilityHistory(Allocator& allocator) noexcept;

    [[nodiscard]] Status resize(u32 slots) noexcept;

    /// Note that a slot was visible this frame.
    void mark_visible(u32 slot) noexcept;

    /// Advance a frame: every slot's age grows by one.
    void advance() noexcept;

    /// True while a slot is inside the hysteresis window and must not be occlusion-culled.
    [[nodiscard]] bool recently_visible(u32 slot, u8 frames) const noexcept;

    void clear() noexcept;

private:
    /// Frames since the slot was last seen, saturating at 255. One byte per slot: a million
    /// instances is a megabyte, which is the right price for not flickering.
    Array<u8> age_;
};

/// The two-pass scheme, as the state a frame carries between its two dispatches.
///
/// `rendering-culling-and-lod`: "a first pass drawing what previous-frame visibility suggests, an
/// HZB rebuilt from that depth, and a second pass testing uncertain or newly visible geometry".
///
/// What this class holds is the DECISION, not the passes: which slots the first pass draws, and
/// therefore which the second must test. The passes themselves are the render graph's, at layer 4.
class TwoPassCull {
public:
    explicit TwoPassCull(Allocator& allocator) noexcept;

    [[nodiscard]] Status resize(u32 slots) noexcept;

    /// Record what the cull found visible this frame, which is what the NEXT frame's first pass
    /// draws.
    void record_visible(Span<const GpuDrawPayload> payloads) noexcept;

    /// Whether the first pass draws a slot: it was visible in the previous frame.
    [[nodiscard]] bool in_first_pass(u32 slot) const noexcept;

    /// Roll the current frame's record into the previous one. Called between frames.
    void advance() noexcept;

    /// Forget everything. What a camera cut does, alongside `Hzb::invalidate()`: with no previous
    /// visibility the first pass draws nothing, the pyramid is empty, and the second pass draws
    /// everything — which is the correct, conservative behaviour for a frame with no history.
    void clear() noexcept;

    [[nodiscard]] u32 first_pass_count() const noexcept { return first_pass_count_; }

private:
    Array<u8> previous_;
    Array<u8> current_;
    u32 first_pass_count_ = 0;
};

}  // namespace cy::render::culling
