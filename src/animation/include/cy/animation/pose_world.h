#pragma once
// The GPU pose world: the shared pose representation every consumer of bone transforms reads, and
// the handoff from pose evaluation to skinning. M8.b tasks 5.3 and 5.4.
//
// ================================================================================================
// WHY THIS EXISTS AT ALL, IN THE WORDS OF THE MODULE THAT HAS BEEN WAITING FOR IT
// ================================================================================================
//
// `src/servers/render/geometry/include/cy/servers/render/geometry/skinning.h`, written at M6:
//
//   "Bone matrices SHALL be read from the GPU pose world (see `animation-and-skinning`), the shared
//   GPU-side pose representation, rather than from a buffer uploaded independently per consumer.
//   [...] There is no GPU pose world at M6, so this module cannot read one, and it must not invent
//   a second: inventing one is precisely the failure the requirement exists to prevent."
//
// This is the one. `PoseSource::GpuPoseWorld` is what `SkinningDescriptor` names, `PoseHandle` is
// what an instance holds, and `matrix_offset()` is what goes in `SkinningDescriptor::pose_offset`.
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS THE RENDERER'S
// ================================================================================================
//
// NO DEVICE, NO BUFFER, NO UPLOAD — the same line `gpu_scene.h` draws one layer down. The world
// owns the PACKED MATRIX ARRAY and the dirty range; the renderer owns the buffer it goes into and
// the frame it goes in. `bytes()`, `upload_offset()` and `upload_size()` are that handoff, and they
// are the same shape `FrameAssembly` already uses for the material table: transfer
// `[upload_offset, upload_offset + upload_size)` before submitting.
//
// CURRENT AND PREVIOUS, BOTH RESIDENT. "holding per skeleton instance the current bone matrices,
// the previous frame's bone matrices, and derived per-bone velocity where required" — an instance's
// two frames are adjacent in the array so a motion-vector pass reads one contiguous range, and
// `publish()` rotates them without moving anything.
//
// ADDED AND REMOVED WITHOUT A REBUILD. "Instances SHALL be added and removed without rebuilding the
// world." A removed range goes on a free list keyed by its size and is reused by the next instance
// of that size; every other handle's offset is untouched, which is what makes a streaming world
// possible at all. A handle carries a generation, so a stale one is refused rather than reading
// whatever moved in.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

#include <cy/animation/skeleton.h>

namespace cy::animation {

/// A skeleton instance's place in the pose world.
struct PoseHandle {
    u32 index = 0xFFFFFFFFU;
    u32 generation = 0;

    [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFU; }
    friend bool operator==(const PoseHandle& a, const PoseHandle& b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
};

struct PoseWorldStats {
    u32 instances = 0;
    u32 live_matrices = 0;
    u32 free_matrices = 0;
    u32 capacity_matrices = 0;
    u32 publishes = 0;
    u32 reused_slots = 0;
};

/// The shared GPU-side pose representation.
class PoseWorld {
public:
    explicit PoseWorld(Allocator& allocator) noexcept;

    /// Reserve room for an instance's bones. The reservation is TWO frames' worth of matrices; a
    /// caller never has to know that.
    [[nodiscard]] Expected<PoseHandle, Error> add(u32 bone_count) noexcept;

    /// Release an instance. Its range is reused by the next instance of the same size; no other
    /// handle moves.
    [[nodiscard]] Status remove(PoseHandle handle) noexcept;

    [[nodiscard]] bool live(PoseHandle handle) const noexcept;

    /// Publish this frame's bone matrices. The previous frame's become the previous set, which is
    /// what motion vectors read; nothing is copied.
    [[nodiscard]] Status publish(PoseHandle handle, Span<const Mat4> matrices) noexcept;

    [[nodiscard]] Span<const Mat4> current(PoseHandle handle) const noexcept;
    [[nodiscard]] Span<const Mat4> previous(PoseHandle handle) const noexcept;

    /// Per-bone velocity, derived from the two frames over `dt`. "derived per-bone velocity where
    /// required" — computed on demand rather than stored, because a consumer that does not need it
    /// should not pay a third array.
    [[nodiscard]] Status velocities(PoseHandle handle, f32 dt, Span<Vec3> out) const noexcept;

    /// Where this instance's CURRENT matrices begin, in matrices. This is
    /// `SkinningDescriptor::pose_offset`, and it changes each time the instance publishes — which
    /// is what double buffering IS. A descriptor is a description built per frame, not a resource
    /// held across frames, so a caller reads this when it builds one.
    [[nodiscard]] u32 matrix_offset(PoseHandle handle) const noexcept;
    /// Where the previous frame's matrices begin. What a motion-vector pass reads.
    [[nodiscard]] u32 previous_offset(PoseHandle handle) const noexcept;
    /// The base of the instance's two-frame range. What a residency report charges.
    [[nodiscard]] u32 base_offset(PoseHandle handle) const noexcept;
    [[nodiscard]] u32 bone_count(PoseHandle handle) const noexcept;

    /// The whole array, for the renderer to transfer.
    [[nodiscard]] Span<const Mat4> matrices() const noexcept { return storage_.span(); }
    [[nodiscard]] u32 upload_offset() const noexcept { return dirty_first_; }
    [[nodiscard]] u32 upload_size() const noexcept {
        return dirty_last_ >= dirty_first_ ? (dirty_last_ - dirty_first_) + 1 : 0;
    }
    /// Called by the renderer once it has transferred the range.
    void clear_upload_range() noexcept;

    [[nodiscard]] const PoseWorldStats& stats() const noexcept { return stats_; }

private:
    struct Slot {
        u32 offset = 0;
        u32 bones = 0;
        u32 generation = 1;
        /// Which of the slot's two halves is the current one.
        u32 parity = 0;
        bool live = false;
    };

    [[nodiscard]] Expected<u32, Error> take_free_range(u32 matrices) noexcept;
    void mark_dirty(u32 first, u32 count) noexcept;
    [[nodiscard]] const Slot* slot_of(PoseHandle handle) const noexcept;

    Array<Mat4> storage_;
    Array<Slot> slots_;
    Array<u32> free_slots_;
    /// Freed ranges, as (offset, matrices) pairs. Reused only by an instance of the same size, so
    /// the array never fragments into unusable holes.
    Array<u64> free_ranges_;
    PoseWorldStats stats_;
    u32 dirty_first_ = 0xFFFFFFFFU;
    u32 dirty_last_ = 0;
};

/// One instance's evaluated pose, taken from local transforms all the way to the pose world.
///
/// "The pipeline SHALL be: sample clips -> blend to a local pose -> resolve to a global pose ->
/// produce bone matrices -> publish -> skin." This is the third, fourth and fifth arrows, and it is
/// a free function rather than a method of anything because the four things it joins belong to four
/// different owners.
[[nodiscard]] Status publish_pose(const Skeleton& skeleton, Span<const Transform> local,
                                  u8 bone_lod, PoseWorld& world, PoseHandle handle,
                                  Span<Transform> model_scratch,
                                  Span<Mat4> matrix_scratch) noexcept;

}  // namespace cy::animation
