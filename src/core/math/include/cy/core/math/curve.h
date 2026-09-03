#pragma once
// Curve, Curve3D and Gradient. Task 3.1.5.
//
// `core-math` — "Curves and easing": `Curve` (1D keyframed with tangents), `Curve2D`/`Curve3D`
// (splines with arc-length parameterisation and baked lookup), and `Gradient`.
//
// The scenario that fixes the design is "constant-speed path following": sampling a `Curve3D` by
// distance uses a **baked arc-length table**, not per-frame integration. A spline's parameter is
// not its arc length — a Catmull-Rom segment between two close control points covers as much `t`
// as one between two distant ones — so an entity moved by `t` speeds up and slows down for reasons
// nothing in the scene explains. The table is built once and sampling by distance is O(1) after it.
//
// STORAGE. These hold `std::vector`. `core-memory-and-containers` (task 2.4) is landing the
// allocator-aware sequence containers in this same milestone; when it does, these three types are
// three declarations to change, and the interface here does not move. Called out rather than left
// to be discovered, because "why does a curve allocate from the global heap" is a fair question.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/color.h>
#include <cy/core/math/vec.h>

#include <vector>

namespace cy {

/// What a curve does outside the range its keys cover.
enum class CurveExtrapolation : u32 {
    /// Hold the first or last value. The default, because it is the only mode that cannot produce
    /// a value the author never saw.
    Clamp = 0,
    /// Continue along the boundary tangent.
    Linear,
    /// Repeat the whole curve.
    Repeat,
    /// Repeat, alternating direction.
    PingPong,
};

/// One keyframe of a 1D curve: a time, a value, and the incoming and outgoing tangents in
/// value-units per time-unit.
///
/// Two tangents rather than one so that a corner is representable. An author who wants a smooth key
/// sets both to the same number; one who wants an instant change of direction does not, and a
/// single-tangent representation would make that impossible to express rather than merely unusual.
struct CurveKey {
    f32 time = 0.0f;
    f32 value = 0.0f;
    f32 in_tangent = 0.0f;
    f32 out_tangent = 0.0f;
};

/// A 1D keyframed curve evaluated by cubic Hermite interpolation between adjacent keys.
class Curve {
public:
    Curve() = default;

    /// Insert a key, keeping the keys ordered by time. Returns its index.
    ///
    /// Two keys at the same time are permitted and produce a step: the segment between them has
    /// zero duration, so the value jumps. That is a legitimate thing to author, and rejecting it
    /// would make a curve unable to express something a designer will certainly ask for.
    usize add_key(const CurveKey& key);

    /// Set every key at once, sorting by time. Cheaper than repeated insertion for a curve arriving
    /// from an asset.
    void set_keys(const CurveKey* keys, usize count);

    [[nodiscard]] f32 evaluate(f32 time) const noexcept;

    /// The derivative at `time`, in value-units per time-unit. Used by anything that needs the rate
    /// as well as the value — a spring, a motion blur vector, a curve editor's tangent handles.
    [[nodiscard]] f32 derivative(f32 time) const noexcept;

    [[nodiscard]] usize key_count() const noexcept { return keys_.size(); }
    [[nodiscard]] const CurveKey& key(usize index) const noexcept { return keys_[index]; }
    [[nodiscard]] const CurveKey* keys() const noexcept { return keys_.data(); }

    /// The time of the first and last key. Both zero when the curve is empty.
    [[nodiscard]] f32 start_time() const noexcept {
        return keys_.empty() ? 0.0f : keys_.front().time;
    }
    [[nodiscard]] f32 end_time() const noexcept { return keys_.empty() ? 0.0f : keys_.back().time; }

    void set_extrapolation(CurveExtrapolation before, CurveExtrapolation after) noexcept {
        before_ = before;
        after_ = after;
    }

    void clear() noexcept { keys_.clear(); }

private:
    /// Map a time outside [start, end] into the range, per the extrapolation mode. Returns false
    /// when the caller should instead hold the boundary value.
    [[nodiscard]] bool wrap_time(f32& time) const noexcept;

    std::vector<CurveKey> keys_;
    CurveExtrapolation before_ = CurveExtrapolation::Clamp;
    CurveExtrapolation after_ = CurveExtrapolation::Clamp;
};

/// A 3D spline through its control points, with a baked arc-length table.
///
/// Centripetal Catmull-Rom (alpha = 0.5), which passes through every control point and — unlike the
/// uniform form — does not produce cusps or self-intersections when two control points are close
/// together. That failure is common in hand-authored paths and looks like a bug in the follower
/// rather than in the curve, which is why the centripetal parameterisation is the default rather
/// than an option.
class Curve3D {
public:
    Curve3D() = default;

    void add_point(Vec3 point);
    void set_points(const Vec3* points, usize count);
    void clear() noexcept;

    [[nodiscard]] usize point_count() const noexcept { return points_.size(); }
    [[nodiscard]] Vec3 point(usize index) const noexcept { return points_[index]; }

    /// Sample by spline parameter, `t` in [0, 1] over the whole curve. Not constant speed — see
    /// `sample_by_distance`.
    [[nodiscard]] Vec3 sample(f32 t) const noexcept;

    /// The tangent (unnormalised velocity) at spline parameter `t`.
    [[nodiscard]] Vec3 tangent(f32 t) const noexcept;

    /// Build the arc-length table. `samples_per_segment` trades table size against how closely the
    /// table's piecewise-linear approximation follows the true arc length; 16 is accurate to well
    /// under a percent for a path a designer would draw.
    ///
    /// Must be called before `length()` or `sample_by_distance()`. Adding a point invalidates it,
    /// and the accessors report `ErrorCode::Unavailable` rather than returning a stale answer.
    Expected<void, Error> bake(u32 samples_per_segment = 16);

    [[nodiscard]] bool is_baked() const noexcept { return !arc_lengths_.empty(); }

    /// Total arc length in metres. Requires a bake.
    [[nodiscard]] Expected<f32, Error> length() const noexcept;

    /// The point `distance` metres along the curve, measured from the start. O(1) after the bake —
    /// a binary search over the table and one lerp, with no integration.
    [[nodiscard]] Expected<Vec3, Error> sample_by_distance(f32 distance) const noexcept;

    /// The spline parameter corresponding to `distance` metres along the curve.
    [[nodiscard]] Expected<f32, Error> parameter_at_distance(f32 distance) const noexcept;

private:
    /// The four control points and the local parameter for the segment containing `t`.
    void segment_for(f32 t, usize& segment, f32& local) const noexcept;

    std::vector<Vec3> points_;
    /// Cumulative arc length at each sample, `arc_lengths_[0] == 0`. Empty until baked.
    std::vector<f32> arc_lengths_;
    /// The spline parameter each entry of `arc_lengths_` was taken at.
    std::vector<f32> arc_parameters_;
};

/// The 2D form. Same spline and the same baked table, in the plane.
class Curve2D {
public:
    Curve2D() = default;

    void add_point(Vec2 point);
    void set_points(const Vec2* points, usize count);
    void clear() noexcept;

    [[nodiscard]] usize point_count() const noexcept { return curve_.point_count(); }
    [[nodiscard]] Vec2 point(usize index) const noexcept;
    [[nodiscard]] Vec2 sample(f32 t) const noexcept;
    [[nodiscard]] Vec2 tangent(f32 t) const noexcept;
    Expected<void, Error> bake(u32 samples_per_segment = 16) {
        return curve_.bake(samples_per_segment);
    }
    [[nodiscard]] bool is_baked() const noexcept { return curve_.is_baked(); }
    [[nodiscard]] Expected<f32, Error> length() const noexcept { return curve_.length(); }
    [[nodiscard]] Expected<Vec2, Error> sample_by_distance(f32 distance) const noexcept;

private:
    /// A `Curve3D` in the z = 0 plane. One spline implementation rather than two that must be kept
    /// in agreement; the wasted float per point is not worth a second copy of the arc-length logic.
    Curve3D curve_;
};

/// A colour ramp: colour keys and, separately, alpha keys.
///
/// Separate because that is how an artist edits one — a gradient widget has two strips — and
/// because the two are genuinely independent: a fade-out changes alpha over a colour ramp that is
/// not moving. Keys are stored in the linear space `Color` uses (color.h), so interpolation is
/// physically meaningful; a gradient authored in sRGB is converted on ingest by the importer.
class Gradient {
public:
    struct ColorKey {
        f32 position = 0.0f;
        Color color = colors::kWhite;
    };
    struct AlphaKey {
        f32 position = 0.0f;
        f32 alpha = 1.0f;
    };

    Gradient() = default;

    void add_color_key(f32 position, const Color& color);
    void add_alpha_key(f32 position, f32 alpha);
    void clear() noexcept;

    /// Colour and alpha at `position`, clamped to [0, 1]. An empty gradient evaluates to opaque
    /// white, which is the neutral value for a multiply and the one that makes a missing gradient
    /// look like a missing gradient rather than like a black hole.
    [[nodiscard]] Color evaluate(f32 position) const noexcept;

    [[nodiscard]] usize color_key_count() const noexcept { return color_keys_.size(); }
    [[nodiscard]] usize alpha_key_count() const noexcept { return alpha_keys_.size(); }

private:
    std::vector<ColorKey> color_keys_;
    std::vector<AlphaKey> alpha_keys_;
};

}  // namespace cy
