#ifndef CY_CAMERA_VOLUME_H
#define CY_CAMERA_VOLUME_H
// Camera volumes, and the index that stops them being tested one by one. M8.b task 7.3.
//
// `camera-system`: "World volumes SHALL be able to influence cameras within them: field of view,
// smoothing, collision policy, lens, composition, and post-process contribution. Volumes SHALL have
// priority and blend distance, and overlapping volumes SHALL be resolved by declared rules, as
// post-process volumes are. Volumes SHALL be found through the world's spatial index rather than by
// testing every volume each frame."
//
// The last sentence is the one with teeth, and a comment cannot hold it. `VolumeSet` keeps a
// uniform grid over the volumes' bounds and `query()` visits only the cell the camera is in;
// `tested()` counts the volumes actually compared against, so the case "a thousand volumes, a
// handful tested" is a measurement rather than an assertion about the implementation. A caller with
// its own world index can bypass this and call `blend_volumes()` with the candidates it found — the
// blend and the index are separate for exactly that reason.

#include <cy/camera/framing.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/rig.h>

namespace cy::camera {

/// What a volume changes about a camera inside it. Every field is an OVERRIDE with a flag, rather
/// than a value that is always applied: a volume that only changes the collision policy must not
/// also silently reset the field of view to a struct default.
struct VolumeSettings {
    bool overrides_fov = false;
    f32 fov_y_radians = 1.0471975512F;

    bool overrides_smoothing = false;
    /// Seconds. The half-life the rig smooths with while the volume applies.
    f32 smoothing_half_life = 0.15F;

    bool overrides_collision = false;
    /// The camera server's own collision parameters — the responses, the probe radius and the
    /// occlusion sample count. A volume that changes the collision policy hands the rig THAT type,
    /// so there is one collision vocabulary in the tree rather than a volume's and a rig's.
    CollisionParams collision;

    bool overrides_composition = false;
    Composition composition;

    bool overrides_distance = false;
    f32 distance = 5.0F;

    /// The post-process contribution's weight. Carried, never interpreted: the post chain is the
    /// renderer's, and a camera that applied it would be deciding a renderer's policy.
    f32 post_process_weight = 0.0F;
};

/// One authored volume.
struct CameraVolume {
    Name name;
    Aabb bounds;
    /// Higher wins where two overlap.
    i32 priority = 0;
    /// Metres. The camera's influence ramps from zero at this distance outside the bounds to one at
    /// the boundary, which is what makes an interior change gradual rather than a step.
    f32 blend_distance = 2.0F;
    VolumeSettings settings;
};

/// One volume's influence this frame, for the inspector: "the active volumes and their weights".
struct VolumeInfluence {
    Name name;
    f32 weight = 0.0F;
    i32 priority = 0;
};

/// The blended result.
struct VolumeBlend {
    VolumeSettings settings;
    /// Zero when no volume applies, in which case `settings` is untouched and every override flag
    /// is false.
    f32 total_weight = 0.0F;
    u32 contributors = 0;
};

/// Blend a set of candidates at a point, by the declared rule: weight by the blend ramp, resolve
/// overlaps by priority, and let the highest-priority non-zero contributor own each overridden
/// field. Equal priorities blend by weight, which is what post-process volumes do and what the
/// requirement points at.
[[nodiscard]] VolumeBlend blend_volumes(Span<const CameraVolume> candidates, Vec3 point,
                                        Array<VolumeInfluence>& influences) noexcept;

/// A uniform-grid index over camera volumes.
class VolumeSet {
public:
    explicit VolumeSet(Allocator& allocator, f32 cell_size = 32.0F) noexcept;

    VolumeSet(const VolumeSet&) = delete;
    VolumeSet& operator=(const VolumeSet&) = delete;
    VolumeSet(VolumeSet&&) noexcept = default;
    VolumeSet& operator=(VolumeSet&&) noexcept = default;

    [[nodiscard]] Status add(const CameraVolume& volume) noexcept;
    void clear() noexcept;
    [[nodiscard]] usize size() const noexcept { return volumes_.size(); }

    /// The volumes whose bounds, grown by their blend distance, contain `point`. Only the grid
    /// cells the point falls in are visited.
    [[nodiscard]] Status query(Vec3 point, Array<CameraVolume>& out) noexcept;

    /// How many volumes the last `query()` actually compared against. The measurement that makes
    /// "rather than by testing every volume each frame" checkable.
    [[nodiscard]] u32 tested() const noexcept { return tested_; }

private:
    struct Bucket {
        u64 key = 0;
        u32 volume = 0;
    };

    /// One grid cell's key: the three coordinates packed into 21 bits each, NOT hashed. A hash
    /// would let two cells collide and a query would then return a distant volume as though the
    /// camera were inside it. The packing is exact for |coordinate| < 2^20, which at the default
    /// 32-metre cell is ±33,000 kilometres; a coordinate outside that wraps, and a world that large
    /// is partitioned rather than gridded.
    [[nodiscard]] static constexpr u64 cell_key(i32 x, i32 y, i32 z) noexcept {
        constexpr u64 kMask = (1ULL << 21U) - 1ULL;
        return ((static_cast<u64>(static_cast<u32>(x)) & kMask) << 42U) |
               ((static_cast<u64>(static_cast<u32>(y)) & kMask) << 21U) |
               (static_cast<u64>(static_cast<u32>(z)) & kMask);
    }

    Array<CameraVolume> volumes_;
    Array<Bucket> buckets_;
    f32 cell_size_ = 32.0F;
    u32 tested_ = 0;
    bool sorted_ = true;
};

}  // namespace cy::camera

#endif  // CY_CAMERA_VOLUME_H
