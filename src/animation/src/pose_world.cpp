#include <cy/animation/pose_world.h>

namespace cy::animation {
namespace {

constexpr u32 kNoIndex = 0xFFFFFFFFU;

[[nodiscard]] u64 encode_range(u32 offset, u32 matrices) noexcept {
    return (static_cast<u64>(offset) << 32U) | static_cast<u64>(matrices);
}

[[nodiscard]] u32 range_offset(u64 encoded) noexcept {
    return static_cast<u32>(encoded >> 32U);
}
[[nodiscard]] u32 range_size(u64 encoded) noexcept {
    return static_cast<u32>(encoded);
}

}  // namespace

PoseWorld::PoseWorld(Allocator& allocator) noexcept
    : storage_(allocator), slots_(allocator), free_slots_(allocator), free_ranges_(allocator) {}

Expected<u32, Error> PoseWorld::take_free_range(u32 matrices) noexcept {
    for (usize index = 0; index < free_ranges_.size(); ++index) {
        if (range_size(free_ranges_[index]) != matrices) {
            continue;
        }
        const u32 offset = range_offset(free_ranges_[index]);
        free_ranges_.remove_unordered(index);
        stats_.free_matrices -= matrices;
        ++stats_.reused_slots;
        return offset;
    }
    const auto offset = static_cast<u32>(storage_.size());
    if (Status sized = storage_.resize(storage_.size() + matrices); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = offset; index < storage_.size(); ++index) {
        storage_[index] = Mat4::identity();
    }
    stats_.capacity_matrices = static_cast<u32>(storage_.size());
    return offset;
}

Expected<PoseHandle, Error> PoseWorld::add(u32 bone_count) noexcept {
    if (bone_count == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an instance with no bones has no pose", 0});
    }
    const u32 matrices = bone_count * 2U;
    Expected<u32, Error> offset = take_free_range(matrices);
    if (!offset) {
        return make_unexpected(offset.error());
    }

    u32 index = kNoIndex;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        index = static_cast<u32>(slots_.size());
        if (Status pushed = slots_.push_back(Slot{}); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Slot& slot = slots_[index];
    slot.offset = *offset;
    slot.bones = bone_count;
    slot.parity = 0;
    slot.live = true;
    ++stats_.instances;
    stats_.live_matrices += matrices;
    return PoseHandle{index, slot.generation};
}

Status PoseWorld::remove(PoseHandle handle) noexcept {
    if (!live(handle)) {
        return fail(ErrorCode::NotFound, "this pose handle names no live instance");
    }
    Slot& slot = slots_[handle.index];
    const u32 matrices = slot.bones * 2U;
    slot.live = false;
    ++slot.generation;
    if (Status pushed = free_ranges_.push_back(encode_range(slot.offset, matrices)); !pushed) {
        return pushed;
    }
    if (Status pushed = free_slots_.push_back(handle.index); !pushed) {
        return pushed;
    }
    --stats_.instances;
    stats_.live_matrices -= matrices;
    stats_.free_matrices += matrices;
    return ok();
}

const PoseWorld::Slot* PoseWorld::slot_of(PoseHandle handle) const noexcept {
    if (!handle.valid() || handle.index >= slots_.size()) {
        return nullptr;
    }
    const Slot& slot = slots_[handle.index];
    if (!slot.live || slot.generation != handle.generation) {
        return nullptr;
    }
    return &slot;
}

bool PoseWorld::live(PoseHandle handle) const noexcept {
    return slot_of(handle) != nullptr;
}

void PoseWorld::mark_dirty(u32 first, u32 count) noexcept {
    if (count == 0) {
        return;
    }
    if (dirty_first_ == kNoIndex || first < dirty_first_) {
        dirty_first_ = first;
    }
    const u32 last = first + count - 1;
    dirty_last_ = last > dirty_last_ ? last : dirty_last_;
}

void PoseWorld::clear_upload_range() noexcept {
    dirty_first_ = kNoIndex;
    dirty_last_ = 0;
}

Status PoseWorld::publish(PoseHandle handle, Span<const Mat4> matrices) noexcept {
    const Slot* found = slot_of(handle);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "this pose handle names no live instance");
    }
    if (matrices.size() < found->bones) {
        return fail(ErrorCode::InvalidArgument,
                    "fewer matrices were published than the instance has bones");
    }
    Slot& slot = slots_[handle.index];
    // The rotation is an index flip. Nothing is copied, and last frame's matrices stay exactly
    // where the motion-vector pass expects them.
    slot.parity ^= 1U;
    const u32 first = slot.offset + (slot.parity * slot.bones);
    for (u32 bone = 0; bone < slot.bones; ++bone) {
        storage_[first + bone] = matrices[bone];
    }
    mark_dirty(first, slot.bones);
    ++stats_.publishes;
    return ok();
}

Span<const Mat4> PoseWorld::current(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    if (slot == nullptr) {
        return {};
    }
    return {storage_.data() + slot->offset + (static_cast<usize>(slot->parity) * slot->bones),
            slot->bones};
}

Span<const Mat4> PoseWorld::previous(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    if (slot == nullptr) {
        return {};
    }
    return {storage_.data() + slot->offset + (static_cast<usize>(slot->parity ^ 1U) * slot->bones),
            slot->bones};
}

Status PoseWorld::velocities(PoseHandle handle, f32 dt, Span<Vec3> out) const noexcept {
    const Slot* slot = slot_of(handle);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "this pose handle names no live instance");
    }
    if (dt <= 0.0F) {
        return fail(ErrorCode::InvalidArgument, "a velocity over no time is not a velocity");
    }
    if (out.size() < slot->bones) {
        return fail(ErrorCode::BufferTooSmall, "one velocity per bone");
    }
    const Span<const Mat4> now = current(handle);
    const Span<const Mat4> then = previous(handle);
    for (u32 bone = 0; bone < slot->bones; ++bone) {
        out[bone] = (now[bone].translation() - then[bone].translation()) * (1.0F / dt);
    }
    return ok();
}

u32 PoseWorld::matrix_offset(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    return slot == nullptr ? 0 : slot->offset + (slot->parity * slot->bones);
}

u32 PoseWorld::previous_offset(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    return slot == nullptr ? 0 : slot->offset + ((slot->parity ^ 1U) * slot->bones);
}

u32 PoseWorld::base_offset(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    return slot == nullptr ? 0 : slot->offset;
}

u32 PoseWorld::bone_count(PoseHandle handle) const noexcept {
    const Slot* slot = slot_of(handle);
    return slot == nullptr ? 0 : slot->bones;
}

Status publish_pose(const Skeleton& skeleton, Span<const Transform> local, u8 bone_lod,
                    PoseWorld& world, PoseHandle handle, Span<Transform> model_scratch,
                    Span<Mat4> matrix_scratch) noexcept {
    const u16 joints = skeleton.joint_count();
    if (local.size() < joints || model_scratch.size() < joints || matrix_scratch.size() < joints) {
        return fail(ErrorCode::BufferTooSmall,
                    "the pose, the model scratch and the matrix scratch are all one per joint");
    }
    if (world.bone_count(handle) < joints) {
        return fail(ErrorCode::InvalidArgument,
                    "this instance was added to the pose world with fewer bones than the skeleton "
                    "has");
    }
    const JointMask& retained = skeleton.retained(bone_lod);
    skeleton.to_model(local, retained, model_scratch);
    skeleton.to_skinning(model_scratch, retained, matrix_scratch);
    return world.publish(handle, matrix_scratch.subspan(0, joints));
}

}  // namespace cy::animation
