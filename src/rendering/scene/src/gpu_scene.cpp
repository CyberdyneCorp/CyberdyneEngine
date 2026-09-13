// The GPU scene's allocation policy and publication bookkeeping. Task 4.1.4.
//
// The header carries the design; this file carries three mechanisms worth reading on their own:
// the free-list allocator, the once-per-frame current-to-previous shift, and dirty coalescing.

#include <cy/rendering/scene/gpu_scene.h>

#include <cy/core/base/assert.h>

#include <cstring>

namespace cy::rendering {
namespace {

/// Instances the scene grows by when the free list cannot satisfy a reservation and the request is
/// smaller than this. Growth is otherwise exactly the request: a producer that reserves 100 000
/// slots gets 100 000, not the next power of two of them.
constexpr u32 kMinimumGrowth = 64;

}  // namespace

const char* producer_kind_name(ProducerKind kind) noexcept {
    switch (kind) {
        case ProducerKind::Extract:
            return "extract";
        case ProducerKind::InstancedMesh:
            return "instanced-mesh";
        case ProducerKind::Vfx:
            return "vfx";
        case ProducerKind::Ui:
            return "ui";
        case ProducerKind::Foliage:
            return "foliage";
        case ProducerKind::Terrain:
            return "terrain";
        case ProducerKind::Water:
            return "water";
        case ProducerKind::VirtualGeometry:
            return "virtual-geometry";
        case ProducerKind::Custom:
            return "custom";
        case ProducerKind::Count:
            break;
    }
    return "unknown";
}

AffineTransform3x4 AffineTransform3x4::from_mat4(const Mat4& matrix) noexcept {
    AffineTransform3x4 out;
    for (usize row = 0; row < 3; ++row) {
        for (usize column = 0; column < 4; ++column) {
            out.m[(row * 4) + column] = matrix.at(row, column);
        }
    }
    return out;
}

Mat4 AffineTransform3x4::to_mat4() const noexcept {
    Mat4 out = Mat4::identity();
    for (usize row = 0; row < 3; ++row) {
        for (usize column = 0; column < 4; ++column) {
            out.at(row, column) = m[(row * 4) + column];
        }
    }
    out.at(3, 0) = 0.0f;
    out.at(3, 1) = 0.0f;
    out.at(3, 2) = 0.0f;
    out.at(3, 3) = 1.0f;
    return out;
}

GpuScene::GpuScene(Allocator& allocator) noexcept
    : allocator_(&allocator),
      instances_(allocator),
      slots_(allocator),
      free_(allocator),
      dirty_(allocator),
      producers_(allocator) {}

// --- Producers -----------------------------------------------------------------------------------

Expected<ProducerHandle, Error> GpuScene::register_producer(ProducerKind kind, const char* name,
                                                            PublicationSite site) noexcept {
    const Expected<u32, Error> slot = producer_generations_.allocate();
    if (!slot) {
        return make_unexpected(slot.error());
    }
    if (*slot >= producers_.size()) {
        if (const Status grown = producers_.resize(*slot + 1); !grown) {
            (void)producer_generations_.release(*slot);
            return make_unexpected(grown.error());
        }
    }
    Producer& producer = producers_[*slot];
    producer = Producer{kind, site, name == nullptr ? "" : name, 0, true};
    return ProducerHandle::from_slot(*slot, producer_generations_.generation_of(*slot));
}

Status GpuScene::retire_producer(ProducerHandle producer) noexcept {
    Producer* record = find_producer(producer);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "producer handle is stale or was never registered");
    }

    // Every slot the producer owns goes back to the free list. Walking the slot table rather than a
    // per-producer range list keeps retirement O(slots) with no second structure to keep in step;
    // at M3 scale that is a scan of a few tens of thousands of `u32`s, and the alternative — a list
    // per producer — is a second place a range can be recorded and therefore a second place the two
    // can disagree.
    const u32 owner = producer.index();
    u32 run_start = 0;
    bool in_run = false;
    for (u32 slot = 0; slot < slots_.size(); ++slot) {
        const bool mine = slots_[slot].owner == owner;
        if (mine) {
            if (instances_[slot].live) {
                --live_;
            }
            instances_[slot].live = false;
            slots_[slot].owner = kNoOwner;
        }
        if (mine && !in_run) {
            run_start = slot;
            in_run = true;
        } else if (!mine && in_run) {
            const InstanceRange run{run_start, slot - run_start};
            free_range(run);
            mark_dirty(run);
            in_run = false;
        }
    }
    if (in_run) {
        const InstanceRange run{run_start, static_cast<u32>(slots_.size()) - run_start};
        free_range(run);
        mark_dirty(run);
    }

    record->live = false;
    record->reserved_slots = 0;
    return producer_generations_.release(producer.index());
}

Expected<ProducerInfo, Error> GpuScene::producer_info(ProducerHandle producer) const noexcept {
    const Producer* record = find_producer(producer);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "producer handle is stale or was never registered");
    }
    ProducerInfo info;
    info.kind = record->kind;
    info.site = record->site;
    info.name = record->name;
    info.reserved_slots = record->reserved_slots;
    info.range_count = 0;
    // Count the maximal runs the producer owns. Reported rather than stored for the same reason
    // retirement scans: one source of truth for "who owns this slot".
    const u32 owner = producer.index();
    bool in_run = false;
    for (u32 slot = 0; slot < slots_.size(); ++slot) {
        const bool mine = slots_[slot].owner == owner;
        if (mine && !in_run) {
            ++info.range_count;
        }
        in_run = mine;
    }
    return info;
}

GpuScene::Producer* GpuScene::find_producer(ProducerHandle producer) noexcept {
    if (!producer_generations_.is_live(producer) || producer.index() >= producers_.size()) {
        return nullptr;
    }
    Producer& record = producers_[producer.index()];
    return record.live ? &record : nullptr;
}

const GpuScene::Producer* GpuScene::find_producer(ProducerHandle producer) const noexcept {
    if (!producer_generations_.is_live(producer) || producer.index() >= producers_.size()) {
        return nullptr;
    }
    const Producer& record = producers_[producer.index()];
    return record.live ? &record : nullptr;
}

// --- Reservation ---------------------------------------------------------------------------------

bool GpuScene::take_free(u32 count, InstanceRange& out) noexcept {
    for (usize i = 0; i < free_.size(); ++i) {
        InstanceRange& block = free_[i];
        if (block.count < count) {
            continue;
        }
        out = InstanceRange{block.first, count};
        if (block.count == count) {
            free_.erase(i);
        } else {
            block.first += count;
            block.count -= count;
        }
        return true;
    }
    return false;
}

Status GpuScene::grow_by(u32 count) noexcept {
    const usize base = instances_.size();
    if (const Status grown = instances_.resize(base + count); !grown) {
        return grown;
    }
    if (const Status grown = slots_.resize(base + count); !grown) {
        return grown;
    }
    for (usize slot = base; slot < slots_.size(); ++slot) {
        slots_[slot] = SlotState{0, kNoOwner};
    }
    return ok();
}

Expected<InstanceRange, Error> GpuScene::reserve(ProducerHandle producer, u32 count) noexcept {
    Producer* record = find_producer(producer);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "producer handle is stale or was never registered");
    }
    if (count == 0) {
        return fail(ErrorCode::InvalidArgument, "a reservation of zero slots has no meaning");
    }

    InstanceRange range{};
    if (!take_free(count, range)) {
        const u32 grow = count > kMinimumGrowth ? count : kMinimumGrowth;
        if (const Status grown = grow_by(grow); !grown) {
            return make_unexpected(grown.error());
        }
        // The block just appended is free by construction, so the same first-fit that failed above
        // now succeeds. Going back through it rather than carving the tail directly means there is
        // one placement rule in this file instead of two.
        free_range(InstanceRange{static_cast<u32>(instances_.size()) - grow, grow});
        const bool taken = take_free(count, range);
        CY_ASSERT_MSG(taken, "a freshly grown block must satisfy the reservation that grew it");
        (void)taken;
    }

    const u32 owner = producer.index();
    for (u32 slot = range.first; slot < range.end(); ++slot) {
        slots_[slot].owner = owner;
        slots_[slot].written_frame = 0;
        instances_[slot] = GpuInstance{};
    }
    record->reserved_slots += count;
    return range;
}

Status GpuScene::release(ProducerHandle producer, InstanceRange range) noexcept {
    if (const Status owned = check_owned(producer, range); !owned) {
        return owned;
    }
    Producer* record = find_producer(producer);
    CY_ASSERT_MSG(record != nullptr, "check_owned() already resolved the producer");

    for (u32 slot = range.first; slot < range.end(); ++slot) {
        if (instances_[slot].live) {
            --live_;
        }
        instances_[slot].live = false;
        slots_[slot].owner = kNoOwner;
    }
    record->reserved_slots -= range.count;
    free_range(range);
    // The released slots are still part of the mirrored buffer, and a consumer that reads them must
    // see `live == false` rather than the instance that used to be there. So the release is a
    // transfer like any other write.
    mark_dirty(range);
    return ok();
}

void GpuScene::free_range(InstanceRange range) noexcept {
    if (range.empty()) {
        return;
    }
    // Insert in address order and coalesce with either neighbour. Keeping the list ordered is what
    // makes coalescing a constant-time test against two entries instead of a scan, and it is what
    // makes a scene that reserves and releases all frame converge back to one block rather than
    // fragmenting forever.
    usize at = 0;
    while (at < free_.size() && free_[at].first < range.first) {
        ++at;
    }

    const bool merge_previous = at > 0 && free_[at - 1].end() == range.first;
    const bool merge_next = at < free_.size() && range.end() == free_[at].first;

    if (merge_previous && merge_next) {
        free_[at - 1].count += range.count + free_[at].count;
        free_.erase(at);
        return;
    }
    if (merge_previous) {
        free_[at - 1].count += range.count;
        return;
    }
    if (merge_next) {
        free_[at].first = range.first;
        free_[at].count += range.count;
        return;
    }
    // Out of memory here would lose the range, which leaks slots rather than corrupting anything.
    // It is reported by `free_slots()` disagreeing with the reservations, and there is nothing
    // better to do: `release()` cannot fail its caller after the slots are already dead.
    if (const Status pushed = free_.push_back(range); !pushed) {
        CY_ASSERT_MSG(false, "the GPU scene's free list could not record a released range");
    }
    for (usize i = free_.size() - 1; i > at; --i) {
        const InstanceRange moved = free_[i - 1];
        free_[i - 1] = free_[i];
        free_[i] = moved;
    }
}

Status GpuScene::check_owned(ProducerHandle producer, InstanceRange range) const noexcept {
    const Producer* record = find_producer(producer);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "producer handle is stale or was never registered");
    }
    if (range.empty()) {
        return fail(ErrorCode::InvalidArgument, "an empty instance range has no meaning");
    }
    if (range.end() > instances_.size()) {
        return fail(ErrorCode::OutOfRange, "instance range reaches past the end of the GPU scene");
    }
    const u32 owner = producer.index();
    for (u32 slot = range.first; slot < range.end(); ++slot) {
        if (slots_[slot].owner != owner) {
            return fail(ErrorCode::PermissionDenied,
                        "instance range is not wholly owned by this producer");
        }
    }
    return ok();
}

// --- Publication ---------------------------------------------------------------------------------

Status GpuScene::write_instances(ProducerHandle producer, InstanceRange range,
                                 Span<const GpuInstance> instances) noexcept {
    if (const Status owned = check_owned(producer, range); !owned) {
        return owned;
    }
    const Producer* record = find_producer(producer);
    CY_ASSERT_MSG(record != nullptr, "check_owned() already resolved the producer");
    if (record->site != PublicationSite::Cpu) {
        return fail(ErrorCode::InvalidArgument,
                    "a GPU-site producer publishes with declare_gpu_written(), not from the CPU");
    }
    if (instances.size() != range.count) {
        return fail(ErrorCode::InvalidArgument,
                    "the instance span and the reserved range have different lengths");
    }

    for (u32 i = 0; i < range.count; ++i) {
        const u32 slot = range.first + i;
        GpuInstance& target = instances_[slot];
        GpuInstance published = instances[i];

        // THE ONCE-PER-FRAME SHIFT. The previous transform and bounds are the scene's to maintain,
        // and they must move exactly once per frame however many times a producer writes: a VFX
        // system that publishes twice in one frame would otherwise report the delta between its own
        // two writes as motion.
        const bool first_write_this_frame = slots_[slot].written_frame != frame_index_;
        const bool was_live = target.live;
        if (!was_live) {
            // A slot's first publication has no history. Setting previous equal to current is what
            // stops a newly spawned instance from smearing across the screen in its first frame.
            published.previous_transform = published.transform;
            published.previous_bounds = published.bounds;
        } else if (first_write_this_frame) {
            published.previous_transform = target.transform;
            published.previous_bounds = target.bounds;
        } else {
            published.previous_transform = target.previous_transform;
            published.previous_bounds = target.previous_bounds;
        }

        published.live = true;
        target = published;
        slots_[slot].written_frame = frame_index_;
        if (!was_live) {
            ++live_;
        }
    }
    mark_dirty(range);
    return ok();
}

Status GpuScene::declare_gpu_written(ProducerHandle producer, InstanceRange range,
                                     const Aabb& conservative_bounds) noexcept {
    if (const Status owned = check_owned(producer, range); !owned) {
        return owned;
    }
    const Producer* record = find_producer(producer);
    CY_ASSERT_MSG(record != nullptr, "check_owned() already resolved the producer");
    if (record->site != PublicationSite::Gpu) {
        return fail(ErrorCode::InvalidArgument,
                    "a CPU-site producer publishes with write_instances()");
    }

    for (u32 slot = range.first; slot < range.end(); ++slot) {
        GpuInstance& target = instances_[slot];
        const bool was_live = target.live;
        target.previous_bounds = was_live ? target.bounds : conservative_bounds;
        target.bounds = conservative_bounds;
        target.flags |= InstanceFlags::GpuAuthored;
        target.live = true;
        slots_[slot].written_frame = frame_index_;
        if (!was_live) {
            ++live_;
        }
    }
    // Deliberately NOT marked dirty. The range's contents are written by a dispatch straight into
    // the device-side buffer; a CPU transfer over the same slots would overwrite what the dispatch
    // produced with a mirror that was never authoritative for them. This asymmetry is the whole
    // point of declaring the publication site, and it is why the site is a producer property rather
    // than a per-call flag someone can forget.
    return ok();
}

void GpuScene::begin_frame() noexcept { ++frame_index_; }

const GpuInstance* GpuScene::instance(u32 slot) const noexcept {
    return slot < instances_.size() ? &instances_[slot] : nullptr;
}

u32 GpuScene::free_slots() const noexcept {
    u32 total = 0;
    for (const InstanceRange& block : free_) {
        total += block.count;
    }
    return total;
}

void GpuScene::mark_dirty(InstanceRange range) noexcept {
    // Coalesce against the last entry only. Publication is overwhelmingly a walk in increasing slot
    // order — a producer fills the range it just reserved — so the cheap test catches nearly every
    // merge, and the transfer that consumes this list is allowed to be conservative anyway.
    if (!dirty_.empty()) {
        InstanceRange& last = dirty_.back();
        if (last.end() == range.first) {
            last.count += range.count;
            return;
        }
        if (last == range) {
            return;
        }
    }
    if (const Status pushed = dirty_.push_back(range); !pushed) {
        // Losing a dirty range would mean a stale instance on the device with nothing to say so.
        // Collapsing to a whole-scene transfer is slower and correct, which is the right trade for
        // an allocation failure on this path.
        dirty_.clear();
        if (const Status all = dirty_.push_back(InstanceRange{0, slot_capacity()}); !all) {
            CY_ASSERT_MSG(false, "the GPU scene could not record a dirty range");
        }
    }
}

}  // namespace cy::rendering
