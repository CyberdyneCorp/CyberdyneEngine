// The GPU scene's slot allocator, dirty list and record helpers. See cy/servers/render/gpu_scene.h.

#include <cy/servers/render/gpu_scene.h>

#include <cy/core/math/scalar.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/matrix.h>

namespace cy::render {
namespace {

constexpr const char* kProducerKindNames[] = {
    "Extract", "InstancedMesh",   "Vfx",    "Ui", "Foliage", "Terrain",
    "Water",   "VirtualGeometry", "Custom",
};
static_assert(sizeof(kProducerKindNames) / sizeof(kProducerKindNames[0]) ==
              static_cast<usize>(ProducerKind::Count));

/// The pattern a slot holds when nothing owns it. `flags` clear means `kInstanceActive` clear,
/// which is the one bit every consumer tests; `layer_mask` clear means no view draws it even if a
/// consumer forgot to test the bit. Belt and braces on purpose: a stale record that renders is a
/// frame of last level's geometry, and it is worth four bytes to make it two independent mistakes.
void make_inactive(GpuInstance& record) noexcept {
    record = GpuInstance{};
}

}  // namespace

const char* producer_kind_name(ProducerKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    return (index < static_cast<usize>(ProducerKind::Count)) ? kProducerKindNames[index]
                                                             : "<invalid>";
}

// --- Record helpers -------------------------------------------------------------------------

/// The 4x3 rows of a placement, in the layout the shader reads.
///
/// `Mat4` is COLUMN-major (its fourth column is the translation), and the record is ROW-major with
/// the constant fourth row dropped, so row r column c is `matrix.at(r, c)`. Written once here
/// because a matrix copied between the two conventions without a transpose is the classic silent
/// corruption, and `Mat4`'s own comment says so.
void fill_rows(f32 (&out)[12], const Transform& transform) noexcept {
    const Mat4 matrix = transform.to_matrix();
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[(row * 4) + column] = matrix.at(row, column);
        }
    }
}

void write_transform(GpuInstance& instance, const Transform& transform) noexcept {
    fill_rows(instance.transform, transform);
}

void write_previous_transform(GpuInstance& instance, const Transform& transform) noexcept {
    fill_rows(instance.previous_transform, transform);
}

Mat4 read_transform(const GpuInstance& instance) noexcept {
    Mat4 matrix = Mat4::identity();
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            matrix.at(row, column) = instance.transform[(row * 4) + column];
        }
    }
    return matrix;
}

void write_bounds(GpuInstance& instance, const Aabb& world_bounds) noexcept {
    // The current bounds become the previous ones first. Doing it here rather than in the caller is
    // what makes "previous bounds are the previous frame's" true by construction: there is no
    // ordering for a caller to get wrong, because there is only one call.
    instance.previous_center[0] = instance.bounds_center[0];
    instance.previous_center[1] = instance.bounds_center[1];
    instance.previous_center[2] = instance.bounds_center[2];
    instance.previous_radius = instance.bounds_radius;

    if (world_bounds.is_empty()) {
        instance.bounds_center[0] = 0.0F;
        instance.bounds_center[1] = 0.0F;
        instance.bounds_center[2] = 0.0F;
        instance.bounds_radius = 0.0F;
        return;
    }
    const Vec3 center = world_bounds.center();
    const Vec3 half = world_bounds.half_extents();
    instance.bounds_center[0] = center.x;
    instance.bounds_center[1] = center.y;
    instance.bounds_center[2] = center.z;
    instance.bounds_radius = length(half);
}

// --- The store ------------------------------------------------------------------------------

GpuScene::GpuScene(Allocator& allocator) noexcept
    : records_(allocator),
      free_(allocator),
      reservations_(allocator),
      producers_(allocator),
      dirty_(allocator) {}

Status GpuScene::initialize(u32 capacity) noexcept {
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "a GPU scene with no slots can hold no instance");
    }
    reset();
    if (Status sized = records_.resize(capacity); !sized) {
        return sized;
    }
    for (GpuInstance& record : records_) {
        make_inactive(record);
    }
    if (Status pushed = free_.push_back(FreeBlock{0, capacity}); !pushed) {
        return pushed;
    }
    return ok();
}

Expected<ProducerId, Error> GpuScene::register_producer(const char* name, ProducerKind kind,
                                                        Residency residency) noexcept {
    if (producers_.size() >= kInvalidProducer) {
        return fail(ErrorCode::OutOfRange, "too many GPU scene producers");
    }
    ProducerInfo info;
    info.name = (name != nullptr) ? name : "";
    info.kind = kind;
    info.residency = residency;
    if (Status pushed = producers_.push_back(info); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<ProducerId>(producers_.size() - 1);
}

const ProducerInfo* GpuScene::producer(ProducerId id) const noexcept {
    return (id < producers_.size()) ? &producers_[id] : nullptr;
}

Status GpuScene::take_block(u32 count, u32& first) noexcept {
    // Best fit rather than first fit. A GPU scene's reservations are long-lived and wildly uneven —
    // one effect asks for a hundred thousand slots and a mesh instance asks for one — and first fit
    // spends the large runs on the small requests, which is exactly the fragmentation that leaves
    // `largest_free_run` too small for the next effect.
    usize best = free_.size();
    for (usize index = 0; index < free_.size(); ++index) {
        if (free_[index].count < count) {
            continue;
        }
        if (best == free_.size() || free_[index].count < free_[best].count) {
            best = index;
        }
    }
    if (best == free_.size()) {
        return fail(ErrorCode::OutOfMemory,
                    "no contiguous run of GPU scene slots is large enough; the store does not grow "
                    "because a device-side producer holds buffer offsets a move would invalidate");
    }
    first = free_[best].first;
    if (free_[best].count == count) {
        free_.erase(best);
    } else {
        free_[best].first += count;
        free_[best].count -= count;
    }
    return ok();
}

Status GpuScene::return_block(u32 first, u32 count) noexcept {
    // Insert in address order, then coalesce with the neighbours. Coalescing on release rather than
    // on allocation is what keeps `largest_free_run` meaningful: a store that only merges lazily
    // reports fragmentation it does not have.
    usize position = 0;
    while (position < free_.size() && free_[position].first < first) {
        ++position;
    }
    if (Status pushed = free_.push_back(FreeBlock{0, 0}); !pushed) {
        return pushed;
    }
    for (usize index = free_.size() - 1; index > position; --index) {
        free_[index] = free_[index - 1];
    }
    free_[position] = FreeBlock{first, count};

    if (position + 1 < free_.size() &&
        free_[position].first + free_[position].count == free_[position + 1].first) {
        free_[position].count += free_[position + 1].count;
        free_.erase(position + 1);
    }
    if (position > 0 &&
        free_[position - 1].first + free_[position - 1].count == free_[position].first) {
        free_[position - 1].count += free_[position].count;
        free_.erase(position);
    }
    return ok();
}

usize GpuScene::find_reservation(u32 first) const noexcept {
    for (usize index = 0; index < reservations_.size(); ++index) {
        if (reservations_[index].first == first) {
            return index;
        }
    }
    return reservations_.size();
}

void GpuScene::clear_records(u32 first, u32 count) noexcept {
    for (u32 slot = first; slot < first + count; ++slot) {
        make_inactive(records_[slot]);
    }
}

Expected<InstanceRange, Error> GpuScene::reserve(ProducerId producer, u32 count) noexcept {
    if (producer >= producers_.size()) {
        return fail(ErrorCode::NotFound, "no such GPU scene producer");
    }
    if (count == 0) {
        return fail(ErrorCode::InvalidArgument, "a reservation of no slots is not a reservation");
    }
    u32 first = 0;
    if (Status taken = take_block(count, first); !taken) {
        return make_unexpected(taken.error());
    }
    if (Status pushed = reservations_.push_back(Reservation{first, count, producer}); !pushed) {
        (void)return_block(first, count);
        return make_unexpected(pushed.error());
    }
    clear_records(first, count);
    high_water_ = math::max(high_water_, first + count);
    producers_[producer].reserved_slots += count;
    ++producers_[producer].reservations;
    // Marked dirty so the inactive pattern reaches the device even if the producer writes nothing:
    // the slots may hold a previous tenant's records, and a cull that ran before the upload would
    // draw them.
    const InstanceRange range{first, count};
    if (Status dirtied = mark_dirty(range); !dirtied) {
        return make_unexpected(dirtied.error());
    }
    return range;
}

Status GpuScene::release(ProducerId producer, InstanceRange range) noexcept {
    if (producer >= producers_.size()) {
        return fail(ErrorCode::NotFound, "no such GPU scene producer");
    }
    const usize index = find_reservation(range.first);
    if (index == reservations_.size()) {
        return fail(ErrorCode::NotFound, "that range was never reserved");
    }
    const Reservation& held = reservations_[index];
    if (held.producer != producer || held.count != range.count) {
        return fail(ErrorCode::PermissionDenied,
                    "that range belongs to a different producer, or is not the range it reserved");
    }
    clear_records(range.first, range.count);
    producers_[producer].reserved_slots -= range.count;
    --producers_[producer].reservations;
    reservations_.erase(index);
    if (Status returned = return_block(range.first, range.count); !returned) {
        return returned;
    }
    // The cleared records must reach the device, or a GPU cull keeps finding the departed effect.
    // This is the whole of "removed without requiring a full rebuild": one range, one upload.
    return mark_dirty(range);
}

Expected<Span<GpuInstance>, Error> GpuScene::writable(ProducerId producer,
                                                      InstanceRange range) noexcept {
    if (producer >= producers_.size()) {
        return fail(ErrorCode::NotFound, "no such GPU scene producer");
    }
    if (producers_[producer].residency != Residency::Cpu) {
        return fail(ErrorCode::PermissionDenied,
                    "this producer's slots are written on the device; writing them from the CPU is "
                    "the round trip the GPU scene exists to make unnecessary");
    }
    const usize index = find_reservation(range.first);
    if (index == reservations_.size() || reservations_[index].producer != producer ||
        reservations_[index].count != range.count) {
        return fail(ErrorCode::NotFound, "that range is not this producer's reservation");
    }
    return Span<GpuInstance>(records_.data() + range.first, range.count);
}

Status GpuScene::mark_dirty(InstanceRange range) noexcept {
    if (range.empty()) {
        return ok();
    }
    if (range.end() > records_.size()) {
        return fail(ErrorCode::OutOfRange, "dirty range reaches past the GPU scene's capacity");
    }
    // Insert in address order and merge with anything it touches. Merging on insert keeps the list
    // proportional to the number of *disjoint* regions rather than to the number of calls, so a
    // producer that marks each instance separately still costs one upload.
    usize position = 0;
    while (position < dirty_.size() && dirty_[position].first < range.first) {
        ++position;
    }
    if (Status pushed = dirty_.push_back(InstanceRange{}); !pushed) {
        return pushed;
    }
    for (usize index = dirty_.size() - 1; index > position; --index) {
        dirty_[index] = dirty_[index - 1];
    }
    dirty_[position] = range;

    while (position + 1 < dirty_.size() && dirty_[position].end() >= dirty_[position + 1].first) {
        const u32 end = (dirty_[position].end() > dirty_[position + 1].end())
                            ? dirty_[position].end()
                            : dirty_[position + 1].end();
        dirty_[position].count = end - dirty_[position].first;
        dirty_.erase(position + 1);
    }
    if (position > 0 && dirty_[position - 1].end() >= dirty_[position].first) {
        const u32 end = (dirty_[position - 1].end() > dirty_[position].end())
                            ? dirty_[position - 1].end()
                            : dirty_[position].end();
        dirty_[position - 1].count = end - dirty_[position - 1].first;
        dirty_.erase(position);
    }
    return ok();
}

const GpuInstance& GpuScene::at(u32 slot) const noexcept {
    CY_ASSERT_MSG(slot < records_.size(), "GPU scene slot out of range");
    return records_[slot];
}

GpuSceneStatistics GpuScene::statistics() const noexcept {
    GpuSceneStatistics stats;
    stats.capacity = static_cast<u32>(records_.size());
    stats.high_water = high_water_;
    stats.free_blocks = static_cast<u32>(free_.size());
    stats.producers = static_cast<u32>(producers_.size());
    for (const FreeBlock& block : free_) {
        stats.largest_free_run = math::max(stats.largest_free_run, block.count);
    }
    for (const Reservation& held : reservations_) {
        stats.reserved_slots += held.count;
    }
    for (const InstanceRange& range : dirty_) {
        stats.dirty_slots += range.count;
    }
    return stats;
}

void GpuScene::reset() noexcept {
    records_.clear();
    free_.clear();
    reservations_.clear();
    producers_.clear();
    dirty_.clear();
    high_water_ = 0;
}

}  // namespace cy::render
