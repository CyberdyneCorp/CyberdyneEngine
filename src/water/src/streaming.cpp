// Water segments bound to world cells, and the profile that decides what they carry. M10 task 2.3.

#include <cy/water/streaming.h>

#include <cmath>
#include <utility>

namespace cy::water {

namespace {

/// What one payload is estimated to cost for one segment, in bytes. Crude on purpose, and the same
/// crudeness `world::CostRates` uses: what the number is for is comparing a client's segment
/// against a server's, and an estimate that is wrong by a constant factor compares correctly.
[[nodiscard]] u64 payload_bytes(WaterPayload payload) noexcept {
    switch (payload) {
        case WaterPayload::Surface:
            // The generated surface mesh: the largest single payload, and the one a server drops.
            return 256U * 1024U;
        case WaterPayload::Foam:
            return 64U * 1024U;
        case WaterPayload::Physics:
            return 32U * 1024U;
        case WaterPayload::Shoreline:
            return 16U * 1024U;
        case WaterPayload::Query:
            return 8U * 1024U;
        case WaterPayload::Audio:
            return 4U * 1024U;
        case WaterPayload::kCount:
            break;
    }
    return 0;
}

[[nodiscard]] u64 mask_bytes(WaterPayloadMask mask) noexcept {
    u64 total = 0;
    for (u32 index = 0; index < static_cast<u32>(WaterPayload::kCount); ++index) {
        const auto payload = static_cast<WaterPayload>(index);
        if (mask.has(payload)) {
            total += payload_bytes(payload);
        }
    }
    return total;
}

}  // namespace

const char* water_payload_name(WaterPayload payload) noexcept {
    switch (payload) {
        case WaterPayload::Query:
            return "query";
        case WaterPayload::Physics:
            return "physics";
        case WaterPayload::Surface:
            return "surface";
        case WaterPayload::Foam:
            return "foam";
        case WaterPayload::Shoreline:
            return "shoreline";
        case WaterPayload::Audio:
            return "audio";
        case WaterPayload::kCount:
            break;
    }
    return "unknown";
}

WaterPayloadMask profile_payloads(world::WorldProfile profile) noexcept {
    switch (profile) {
        case world::WorldProfile::DedicatedServer: {
            // "Server keeps queries, drops surfaces." A dedicated server floats boats, paths
            // swimmers, answers depth queries and simulates the flow that carries debris — so it
            // keeps the query state, the physics representation and the shoreline data, and drops
            // the surface geometry, the foam coverage and the audio emitters.
            WaterPayloadMask mask;
            mask.set(WaterPayload::Query);
            mask.set(WaterPayload::Physics);
            mask.set(WaterPayload::Shoreline);
            return mask;
        }
        case world::WorldProfile::Client:
        case world::WorldProfile::Editor:
            break;
    }
    return WaterPayloadMask::all();
}

WaterStreaming::WaterStreaming(Allocator& allocator, const WaterRegistry& registry,
                               const world::PartitionConfig& partition) noexcept
    : allocator_(&allocator),
      registry_(&registry),
      partition_(&partition),
      segments_(allocator),
      bindings_(allocator),
      cells_(allocator),
      drained_(allocator) {}

WaterSegmentKey WaterStreaming::segment_of(WaterBodyId body, f32 segment_metres,
                                           const world::WorldVec3d& at) noexcept {
    const auto metres = static_cast<f64>((segment_metres > 0.0F) ? segment_metres : 1.0F);
    WaterSegmentKey key;
    key.body = body;
    key.x = static_cast<i32>(std::floor(at.x / metres));
    key.z = static_cast<i32>(std::floor(at.z / metres));
    return key;
}

Status WaterStreaming::attach(world::CellEventQueue& events, u32 order) noexcept {
    Expected<world::CellEventQueue::ConsumerId, Error> consumer =
        events.add_consumer("water.streaming", order);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    events_ = &events;
    consumer_ = *consumer;
    return ok();
}

Status WaterStreaming::declare_cell(world::CellId cell, const world::CellCoord& coord) noexcept {
    if (!cell.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "water: a cell must have a valid identity");
    }
    if (world::CellCoord* existing = cells_.find(cell); existing != nullptr) {
        *existing = coord;
        return ok();
    }
    if (Expected<world::CellCoord*, Error> inserted = cells_.insert(cell, coord); !inserted) {
        return make_unexpected(inserted.error());
    }
    return ok();
}

usize WaterStreaming::find_segment(const WaterSegmentKey& key) const noexcept {
    for (usize index = 0; index < segments_.size(); ++index) {
        if (segments_[index].key == key) {
            return index;
        }
    }
    return segments_.size();
}

const WaterSegment* WaterStreaming::find(const WaterSegmentKey& key) const noexcept {
    const usize index = find_segment(key);
    return (index == segments_.size()) ? nullptr : &segments_[index];
}

WaterStreaming::Binding* WaterStreaming::find_binding(world::CellId cell) noexcept {
    for (Binding& binding : bindings_) {
        if (binding.cell == cell) {
            return &binding;
        }
    }
    return nullptr;
}

Status WaterStreaming::reference_segment(const WaterSegmentKey& key,
                                         WaterStreamingReport& report) noexcept {
    const usize index = find_segment(key);
    if (index != segments_.size()) {
        ++segments_[index].references;
        return ok();
    }
    WaterSegment segment;
    segment.key = key;
    segment.payloads = profile_payloads(profile_);
    segment.references = 1;
    segment.bytes = mask_bytes(segment.payloads);
    if (Status pushed = segments_.push_back(segment); !pushed) {
        return pushed;
    }
    ++report.segments_created;
    return ok();
}

void WaterStreaming::release_segment(const WaterSegmentKey& key,
                                     WaterStreamingReport& report) noexcept {
    const usize index = find_segment(key);
    if (index == segments_.size()) {
        return;
    }
    if (segments_[index].references > 1) {
        --segments_[index].references;
        return;
    }
    // The last cell overlapping it released it. "Evicted with them", and it is a consequence of
    // consuming the same events rather than of a second lifetime kept in step.
    segments_.remove_unordered(index);
    ++report.segments_dropped;
}

Status WaterStreaming::bind_body(const WaterBodyRecord& record, const Footprint& footprint,
                                 Binding& binding, WaterStreamingReport& report) noexcept {
    // The segments of this body that the cell overlaps. A body whose segment is larger than a cell
    // contributes one; a body whose segment is smaller contributes several, and a river crossing
    // hundreds of cells contributes a few per cell and stays ONE body throughout.
    const WaterBounds& bounds = record.desc.bounds;
    const auto metres = static_cast<f64>(record.desc.segment_metres);
    const f64 from_x = (footprint.min_x > bounds.min_x) ? footprint.min_x : bounds.min_x;
    const f64 to_x = (footprint.max_x < bounds.max_x) ? footprint.max_x : bounds.max_x;
    const f64 from_z = (footprint.min_z > bounds.min_z) ? footprint.min_z : bounds.min_z;
    const f64 to_z = (footprint.max_z < bounds.max_z) ? footprint.max_z : bounds.max_z;
    const auto first_x = static_cast<i64>(std::floor(from_x / metres));
    const auto last_x = static_cast<i64>(std::floor((to_x - 1e-6) / metres));
    const auto first_z = static_cast<i64>(std::floor(from_z / metres));
    const auto last_z = static_cast<i64>(std::floor((to_z - 1e-6) / metres));

    for (i64 z = first_z; z <= last_z; ++z) {
        for (i64 x = first_x; x <= last_x; ++x) {
            WaterSegmentKey key;
            key.body = record.id;
            key.x = static_cast<i32>(x);
            key.z = static_cast<i32>(z);
            if (Status referenced = reference_segment(key, report); !referenced) {
                return referenced;
            }
            if (Status pushed = binding.segments.push_back(key); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status WaterStreaming::bind_cell(world::CellId cell, WaterStreamingReport& report) noexcept {
    if (find_binding(cell) != nullptr) {
        return ok();
    }
    const world::CellCoord* coord = cells_.find(cell);
    if (coord == nullptr) {
        // A cell nobody declared a footprint for. Not an error: a world may stream cells this
        // module was never told about, and guessing their extent from an opaque identifier is
        // exactly what `world::CellId` being opaque forbids.
        return ok();
    }

    const f64 size = partition_->cell_size(coord->level);
    const world::WorldVec3d origin = world::simulation_origin(*partition_, *coord);
    const Footprint footprint{origin.x, origin.z, origin.x + size, origin.z + size};

    Binding binding(*allocator_);
    binding.cell = cell;
    bool any = false;
    for (const WaterBodyRecord& record : registry_->bodies()) {
        if (!footprint.overlaps(record.desc.bounds)) {
            continue;
        }
        any = true;
        if (Status bound = bind_body(record, footprint, binding, report); !bound) {
            return bound;
        }
    }
    if (!any) {
        ++report.cells_without_water;
    }
    ++report.cells_bound;
    return bindings_.push_back(std::move(binding));
}

Status WaterStreaming::release_cell(world::CellId cell, WaterStreamingReport& report) noexcept {
    for (usize index = 0; index < bindings_.size(); ++index) {
        if (!(bindings_[index].cell == cell)) {
            continue;
        }
        for (const WaterSegmentKey& key : bindings_[index].segments) {
            release_segment(key, report);
        }
        bindings_.remove_unordered(index);
        ++report.cells_released;
        return ok();
    }
    return ok();
}

Expected<WaterStreamingReport, Error> WaterStreaming::tick() noexcept {
    WaterStreamingReport report;
    if (events_ == nullptr) {
        return report;
    }
    drained_.clear();
    if (Status drained = events_->drain(consumer_, drained_); !drained) {
        return make_unexpected(drained.error());
    }

    for (const world::CellEvent& event : drained_) {
        switch (event.kind) {
            case world::CellEventKind::Resident:
            case world::CellEventKind::Activated:
            case world::CellEventKind::ChannelsChanged: {
                // The channel is the gate. A cell that streams geometry but not water binds no
                // segments, which is what makes water's residency a channel decision rather than a
                // second streaming policy.
                if (!event.channels.has(world::Channel::Water)) {
                    if (Status released = release_cell(event.cell, report); !released) {
                        return make_unexpected(released.error());
                    }
                    break;
                }
                if (Status bound = bind_cell(event.cell, report); !bound) {
                    return make_unexpected(bound.error());
                }
                break;
            }
            case world::CellEventKind::Deactivated:
                // Deactivation withdraws a cell's ENTITIES; the cell is still resident and its
                // water still answers queries. Releasing here would drop the surface a boat is
                // floating on because the boat's cell stopped simulating its props.
                break;
            case world::CellEventKind::Evicted:
            case world::CellEventKind::Failed: {
                if (Status released = release_cell(event.cell, report); !released) {
                    return make_unexpected(released.error());
                }
                break;
            }
        }
    }
    return report;
}

bool WaterStreaming::payload_resident(WaterBodyId body, const world::WorldVec3d& at,
                                      WaterPayload payload) const noexcept {
    const WaterBodyRecord* record = registry_->find(body);
    if (record == nullptr) {
        return false;
    }
    const WaterSegment* segment = find(segment_of(body, record->desc.segment_metres, at));
    return segment != nullptr && segment->payloads.has(payload);
}

u64 WaterStreaming::bytes() const noexcept {
    u64 total = 0;
    for (const WaterSegment& segment : segments_) {
        total += segment.bytes;
    }
    return total;
}

}  // namespace cy::water
