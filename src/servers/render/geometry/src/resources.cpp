#include <cy/servers/render/geometry/resources.h>

namespace cy::render::geometry {

const char* memory_category_name(MemoryCategory category) noexcept {
    switch (category) {
        case MemoryCategory::Textures:
            return "textures";
        case MemoryCategory::Meshes:
            return "meshes";
        case MemoryCategory::RenderTargets:
            return "render-targets";
        case MemoryCategory::Buffers:
            return "buffers";
        case MemoryCategory::AccelerationStructures:
            return "acceleration-structures";
        case MemoryCategory::SkinnedVertices:
            return "skinned-vertices";
        case MemoryCategory::Count:
            break;
    }
    return "buffers";
}

const char* texture_residency_name(TextureResidency residency) noexcept {
    switch (residency) {
        case TextureResidency::FullyResident:
            return "fully-resident";
        case TextureResidency::MipStreamed:
            return "mip-streamed";
        case TextureResidency::VirtualStreamed:
            return "virtual-streamed";
        case TextureResidency::VirtualRuntime:
            return "virtual-runtime";
    }
    return "fully-resident";
}

u64 MemoryReport::total_bytes() const noexcept {
    u64 total = 0;
    for (const u64 category : bytes) {
        total += category;
    }
    return total;
}

// --- ResourceLedger -----------------------------------------------------------------------------

ResourceLedger::ResourceLedger(Allocator& allocator) noexcept
    : entries_(allocator), free_(allocator) {}

Status ResourceLedger::set_budget(MemoryCategory category, u64 bytes) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kMemoryCategoryCount) {
        return fail(ErrorCode::OutOfRange, "no such memory category");
    }
    budget_[index] = bytes;
    return ok();
}

Expected<ResourceId, Error> ResourceLedger::acquire(MemoryCategory category, u64 bytes,
                                                    bool streamable) noexcept {
    const auto index = static_cast<u32>(category);
    if (index >= kMemoryCategoryCount) {
        return make_unexpected(Error{ErrorCode::OutOfRange, "no such memory category"});
    }

    Entry entry;
    entry.bytes = bytes;
    entry.references = 1;
    entry.category = category;
    entry.streamable = streamable;
    entry.live = true;

    ResourceId id = kInvalidResource;
    if (!free_.empty()) {
        id = free_[free_.size() - 1];
        free_.pop_back();
        entries_[id] = entry;
    } else {
        id = static_cast<ResourceId>(entries_.size());
        if (Status pushed = entries_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    // A budget REPORTS rather than refuses. Refusing an allocation mid-frame produces a missing
    // object; the requirement's answer to pressure is eviction, and `evict_candidates` is where
    // that is served from.
    if (budget_[index] != 0) {
        u64 charged = 0;
        for (const Entry& other : entries_) {
            if (other.live && static_cast<u32>(other.category) == index) {
                charged += other.bytes;
            }
        }
        if (charged > budget_[index]) {
            ++budget_exceeded_;
        }
    }
    return id;
}

Status ResourceLedger::add_reference(ResourceId id) noexcept {
    if (id >= entries_.size() || !entries_[id].live) {
        return fail(ErrorCode::NotFound, "no such GPU resource");
    }
    if (entries_[id].pending) {
        return fail(ErrorCode::Unavailable,
                    "this resource's last reference has already gone; it is waiting for the device "
                    "to finish with it and may not be revived");
    }
    ++entries_[id].references;
    return ok();
}

Status ResourceLedger::release(ResourceId id, u64 frame) noexcept {
    if (id >= entries_.size() || !entries_[id].live) {
        return fail(ErrorCode::NotFound, "no such GPU resource");
    }
    Entry& entry = entries_[id];
    if (entry.references == 0) {
        return fail(ErrorCode::InvalidArgument, "this resource has no reference left to release");
    }
    --entry.references;
    if (entry.references == 0) {
        // NOT freed here. The device may still be reading it for as many frames as are in flight,
        // and this is the one rule whose wrong version works in testing.
        entry.pending = true;
        entry.released_frame = frame;
    }
    return ok();
}

u64 ResourceLedger::retire(u64 completed_frame) noexcept {
    u64 reclaimed = 0;
    for (usize index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (!entry.live || !entry.pending) {
            continue;
        }
        if (entry.released_frame + frames_in_flight_ > completed_frame) {
            continue;
        }
        reclaimed += entry.bytes;
        entry.live = false;
        entry.pending = false;
        (void)free_.push_back(static_cast<ResourceId>(index));
    }
    released_ += reclaimed;
    return reclaimed;
}

u32 ResourceLedger::reference_count(ResourceId id) const noexcept {
    return id < entries_.size() && entries_[id].live ? entries_[id].references : 0;
}

bool ResourceLedger::live(ResourceId id) const noexcept {
    return id < entries_.size() && entries_[id].live;
}

u64 ResourceLedger::bytes_of(ResourceId id) const noexcept {
    return id < entries_.size() && entries_[id].live ? entries_[id].bytes : 0;
}

u32 ResourceLedger::evict_candidates(MemoryCategory category, Span<ResourceId> out) const noexcept {
    u32 written = 0;
    for (usize index = 0; index < entries_.size() && written < out.size(); ++index) {
        const Entry& entry = entries_[index];
        if (!entry.live || entry.pending || !entry.streamable || entry.category != category) {
            continue;
        }
        // Insertion sort into the output, largest first. The list is short by construction — a
        // caller asks for as many as it is willing to consider — so a sort over it is cheaper than
        // building and sorting the whole category.
        u32 position = written;
        while (position > 0 && entries_[out[position - 1]].bytes < entry.bytes) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = static_cast<ResourceId>(index);
        ++written;
    }
    return written;
}

MemoryReport ResourceLedger::report() const noexcept {
    MemoryReport report;
    for (u32 index = 0; index < kMemoryCategoryCount; ++index) {
        report.budget[index] = budget_[index];
    }
    for (const Entry& entry : entries_) {
        if (!entry.live) {
            continue;
        }
        const auto index = static_cast<u32>(entry.category);
        report.bytes[index] += entry.bytes;
        report.resources[index] += 1;
        if (entry.pending) {
            report.pending_release += entry.bytes;
        }
    }
    report.released = released_;
    report.budget_exceeded = budget_exceeded_;
    return report;
}

// --- MipChain -----------------------------------------------------------------------------------

Status MipChain::configure(TextureResidency residency, u8 mip_count, u8 tail,
                           u64 bytes_of_mip_zero) noexcept {
    if (residency == TextureResidency::VirtualStreamed ||
        residency == TextureResidency::VirtualRuntime) {
        return fail(ErrorCode::Unsupported,
                    "a virtual residency model is governed by virtual-texturing, not by mip "
                    "streaming");
    }
    if (mip_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a texture has at least one mip");
    }
    if (tail == 0) {
        // The guarantee, refused rather than defaulted: "The lowest few mips SHALL always be
        // resident so no texture is ever entirely missing."
        return fail(ErrorCode::InvalidArgument,
                    "a mip tail of zero would let a texture be entirely missing from a frame");
    }
    if (tail > mip_count) {
        return fail(ErrorCode::InvalidArgument, "the mip tail is longer than the chain");
    }
    residency_ = residency;
    mip_count_ = mip_count;
    tail_ = tail;
    // The tail is resident from the moment the texture exists. The highest resident level is
    // therefore the top of the tail — the smallest mips are the highest-numbered ones.
    const u8 tail_top = static_cast<u8>(mip_count - tail);
    highest_resident_ = residency == TextureResidency::FullyResident ? 0 : tail_top;
    requested_ = highest_resident_;
    bytes_of_mip_zero_ = bytes_of_mip_zero;
    return ok();
}

void MipChain::request(u8 level) noexcept {
    if (residency_ != TextureResidency::MipStreamed) {
        return;
    }
    const u8 clamped = level < mip_count_ ? level : static_cast<u8>(mip_count_ - 1U);
    requested_ = clamped;
}

void MipChain::commit_resident(u8 level) noexcept {
    if (residency_ != TextureResidency::MipStreamed) {
        return;
    }
    const u8 tail_top = static_cast<u8>(mip_count_ - tail_);
    // Never above the tail: evicting into the tail is what would make a texture missing.
    highest_resident_ = level < tail_top ? level : tail_top;
}

u8 MipChain::resident_mip(u8 wanted) const noexcept {
    // A request for a level that is not resident answers the highest resident one. This is the
    // whole of "never block the frame — a non-resident mip SHALL fall back to the highest resident
    // one", and it is why this function cannot fail.
    if (wanted >= mip_count_) {
        return static_cast<u8>(mip_count_ - 1U);
    }
    return wanted > highest_resident_ ? wanted : highest_resident_;
}

u64 MipChain::bytes_for(u8 level) const noexcept {
    // A halving chain: mip n is a quarter of mip n-1, so residency down to `level` costs the sum
    // from `level` to the smallest.
    u64 total = 0;
    u64 size = bytes_of_mip_zero_;
    for (u8 mip = 0; mip < mip_count_; ++mip) {
        if (mip >= level) {
            total += size > 0 ? size : 1;
        }
        size /= 4;
    }
    return total;
}

// --- GeometryRing -------------------------------------------------------------------------------

Status GeometryRing::configure(u64 slice_bytes, u32 frames) noexcept {
    if (slice_bytes == 0 || frames == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a geometry ring needs a positive slice and at least one frame in flight");
    }
    slice_bytes_ = slice_bytes;
    frames_ = frames;
    used_ = 0;
    peak_ = 0;
    overflows_ = 0;
    slice_ = 0;
    return ok();
}

void GeometryRing::begin_frame(u64 frame) noexcept {
    slice_ = frames_ == 0 ? 0 : static_cast<u32>(frame % frames_);
    used_ = 0;
}

Expected<u64, Error> GeometryRing::allocate(u64 bytes, u64 alignment) noexcept {
    if (slice_bytes_ == 0) {
        return make_unexpected(
            Error{ErrorCode::Unavailable, "the geometry ring is not configured"});
    }
    const u64 align = alignment == 0 ? 1 : alignment;
    const u64 aligned = (used_ + align - 1) / align * align;
    if (aligned + bytes > slice_bytes_) {
        // Refused rather than wrapped. Wrapping into the next slice is how a frame overwrites the
        // vertices the device is still reading, and the symptom is geometry that flickers on one
        // machine and not another.
        ++overflows_;
        return make_unexpected(
            Error{ErrorCode::OutOfMemory,
                  "this frame's slice of the geometry ring is full; the ring is too small, and "
                  "wrapping into the next slice would overwrite a frame still in flight"});
    }
    used_ = aligned + bytes;
    peak_ = used_ > peak_ ? used_ : peak_;
    return (static_cast<u64>(slice_) * slice_bytes_) + aligned;
}

// --- Statistics ---------------------------------------------------------------------------------

void GeometryStatistics::clear() noexcept {
    *this = GeometryStatistics{};
}

void GeometryStatistics::merge(const GeometryStatistics& other) noexcept {
    triangles += other.triangles;
    vertices += other.vertices;
    draws += other.draws;
    for (u32 index = 0; index < 8; ++index) {
        lod_histogram[index] += other.lod_histogram[index];
    }
    for (u32 index = 0; index < 16; ++index) {
        mip_histogram[index] += other.mip_histogram[index];
    }
    vertex_bytes += other.vertex_bytes;
    // Overdraw is a ratio, so the merge is a draw-weighted mean rather than a sum: adding two
    // overdraw figures would report four on two views that each overdrew twice.
    const u64 total_draws = static_cast<u64>(draws);
    if (total_draws != 0) {
        const u64 weighted = (static_cast<u64>(overdraw_fixed_point) * (draws - other.draws)) +
                             (static_cast<u64>(other.overdraw_fixed_point) * other.draws);
        overdraw_fixed_point = static_cast<u32>(weighted / total_draws);
    }
}

}  // namespace cy::render::geometry
