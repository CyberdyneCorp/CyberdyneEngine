#pragma once
// Unified render importance: published once per instance, consumed by every quality decision.
// Task 4.1.
//
// `residency` — "Unified render importance". A renderable instance carries **one** importance
// value, derived from screen coverage combined with a gameplay-supplied component, and every
// consumer — geometry detail, texture page priority, shadow resolution and refresh rate, animation
// update rate, illumination quality — reads that one value. "Subsystems SHALL NOT maintain
// independent notions of importance for the same instance. Where a subsystem needs a different
// weighting, it SHALL apply a
// **declared transform** of the shared value rather than compute its own."
//
// --- WHY A TRANSFORM IS A STRUCT AND NOT A CALLBACK
// -----------------------------------------------
//
// The requirement is not "each subsystem may adjust importance"; it is that the adjustment is
// *declared*, so that a report can say why shadows scored this instance differently from textures.
// A callback would satisfy the letter and lose exactly that. `ImportanceTransform` is four numbers
// a diagnostic can print, and `apply()` is the only thing that reads them — which is what makes "a
// hero unit is important once" checkable rather than aspirational.
//
// --- THE DISTANCE TERM IS SHARED, TOO
// -------------------------------------------------------------
//
// `residency`'s own example is "shadows weight distance more strongly than textures do". If
// distance were each subsystem's own measurement they would disagree about it, which is the failure
// the whole requirement exists to prevent. So the *inputs* carry a normalised distance alongside
// coverage, the shared value folds it in once, and a transform may only re-weight the shared
// distance — it cannot supply its own.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/residency/types.h>

namespace cy::residency {

/// What an instance publishes. All three are normalised to [0, 1] and clamped on the way in, so a
/// caller that measures coverage in pixels or distance in metres converts once, where it knows the
/// view, rather than here where nothing does.
struct ImportanceInputs {
    /// Fraction of the view the instance covers, in [0, 1].
    f32 screen_coverage = 0.0F;
    /// The gameplay-supplied component, in [0, 1]. "Systems can mark what matters without knowing
    /// how each consumer will use it" — this is that mark.
    f32 gameplay_importance = 0.0F;
    /// 0 at the camera, 1 at the far end of the streaming range. Shared for the reason in the
    /// header comment: a per-subsystem distance is a per-subsystem importance wearing a hat.
    f32 distance_normalised = 0.0F;
};

/// The shared value, in [0, 1]. One function, so that "importance is computed in one place" is a
/// property of the build.
[[nodiscard]] f32 compute_render_importance(const ImportanceInputs& inputs) noexcept;

/// A consumer's declared re-weighting of the shared value.
///
/// The identity transform — gain 1, bias 0, no extra distance weight, exponent 1 — returns the
/// shared value unchanged, which is what a subsystem that has no opinion gets by default.
struct ImportanceTransform {
    f32 gain = 1.0F;
    f32 bias = 0.0F;
    /// Extra emphasis on the SHARED distance term, in [0, 1]. At 1 an instance at the far plane
    /// scores zero however important it is; at 0 distance has only the influence the shared value
    /// already gave it.
    f32 distance_weight = 0.0F;
    /// Applied last. Below 1 it flattens the range, above 1 it sharpens it. `residency`'s example —
    /// shadows caring about the top of the range and nothing else — is an exponent of 2.
    f32 exponent = 1.0F;
};

/// Apply a declared transform. Result is clamped to [0, 1].
[[nodiscard]] f32 apply_transform(const ImportanceTransform& transform, f32 shared,
                                  f32 distance_normalised) noexcept;

/// Published importance, per instance, with one declared transform per subsystem.
///
/// The instance id is opaque here for the same reason a `PageKey`'s page number is: this layer is
/// below the ECS and below the scene graph and may not name an entity. The renderer publishes
/// whatever identity it addresses instances by.
class ImportanceTable {
public:
    explicit ImportanceTable(Allocator& allocator = current_allocator()) noexcept
        : instances_(allocator) {}

    ImportanceTable(const ImportanceTable&) = delete;
    ImportanceTable& operator=(const ImportanceTable&) = delete;
    ImportanceTable(ImportanceTable&&) noexcept = default;
    ImportanceTable& operator=(ImportanceTable&&) noexcept = default;
    ~ImportanceTable() = default;

    /// Declare how one subsystem weights the shared value. Replaces any previous declaration.
    Status declare_transform(Subsystem subsystem, const ImportanceTransform& transform) noexcept;
    [[nodiscard]] const ImportanceTransform& transform(Subsystem subsystem) const noexcept;

    /// Publish one instance's inputs. This is the *only* place importance enters the engine.
    Status publish(u64 instance, const ImportanceInputs& inputs) noexcept;
    bool retire(u64 instance) noexcept;
    void clear() noexcept;

    [[nodiscard]] usize size() const noexcept { return instances_.size(); }

    /// The shared value. Zero for an instance that has published nothing — which is the honest
    /// answer, since an unpublished instance is one no view has measured.
    [[nodiscard]] f32 shared(u64 instance) const noexcept;

    /// The shared value through `subsystem`'s declared transform. This is what a request carries.
    [[nodiscard]] f32 for_subsystem(u64 instance, Subsystem subsystem) const noexcept;

    [[nodiscard]] const ImportanceInputs* inputs(u64 instance) const noexcept;

private:
    HashMap<u64, ImportanceInputs> instances_;
    ImportanceTransform transforms_[kSubsystemCount] = {};
};

}  // namespace cy::residency
