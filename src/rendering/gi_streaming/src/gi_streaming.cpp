// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_streaming/gi_streaming.h>

#include <algorithm>
#include <utility>

namespace cy::rendering::gi_streaming {
namespace {

/// The cards of `cards[first, end)` as one box, a margin of a card wide about it.
[[nodiscard]] Aabb bounds_of(Span<const gi::Surfel> cards) noexcept {
    Aabb box = Aabb::from_min_max(cards[0].position, cards[0].position);
    f32 margin = 0.0F;
    for (const gi::Surfel& card : cards) {
        box = Aabb::from_min_max(cwise_min(box.min, card.position),
                                 cwise_max(box.max, card.position));
        margin = std::max(margin, card.area);
    }
    const f32 pad = std::max(0.5F, margin);
    return Aabb::from_min_max(box.min - Vec3{pad, pad, pad}, box.max + Vec3{pad, pad, pad});
}

/// Where the chunk that starts at `first` ends: as many whole instances as fit in `budget` cards,
/// and at least one instance however large, so an emitter (an instance and material's cards) is
/// classified whole.
[[nodiscard]] usize chunk_end(Span<const gi::Surfel> cards, usize first, u32 budget) noexcept {
    usize end = first;
    while (end < cards.size()) {
        usize run = end + 1;
        while (run < cards.size() && cards[run].instance_id == cards[end].instance_id) {
            ++run;
        }
        if (end != first && run - first > budget) {
            break;
        }
        end = run;
        if (end - first >= budget) {
            break;
        }
    }
    return end;
}

}  // namespace

GiCellStreaming::GiCellStreaming(Allocator& allocator, gi::IlluminationSystem& system,
                                 const world::PartitionConfig& partition) noexcept
    : allocator_(&allocator),
      system_(&system),
      partition_(&partition),
      cells_(allocator),
      drained_(allocator) {}

GiCellStreaming::~GiCellStreaming() = default;

Status GiCellStreaming::attach(world::CellEventQueue& events, u32 order) noexcept {
    if (events_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "GiCellStreaming: already attached");
    }
    Expected<world::CellEventQueue::ConsumerId, Error> consumer =
        events.add_consumer("global illumination", order);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    consumer_ = *consumer;
    events_ = &events;
    return ok();
}

GiCellStreaming::Cell* GiCellStreaming::find(world::CellId cell) noexcept {
    for (Cell& entry : cells_) {
        if (entry.id == cell) {
            return &entry;
        }
    }
    return nullptr;
}

const GiCellStreaming::Cell* GiCellStreaming::find(world::CellId cell) const noexcept {
    for (const Cell& entry : cells_) {
        if (entry.id == cell) {
            return &entry;
        }
    }
    return nullptr;
}

bool GiCellStreaming::resident(world::CellId cell) const noexcept {
    const Cell* entry = find(cell);
    return entry != nullptr && entry->complete;
}

Status GiCellStreaming::bind_cell(world::CellId cell, GiStreamingReport& report) noexcept {
    if (find(cell) != nullptr) {
        return ok();
    }
    Cell entry(*allocator_);
    entry.id = cell;
    if (loader_ != nullptr) {
        if (Status loaded = loader_(loader_user_, cell, *partition_, entry.content); !loaded) {
            report.loader_failures += 1;
            return ok();
        }
    }
    // Cards in instance order, so a chunk is cut between instances (`chunk_end`) and an emitter's
    // cards are classified together. Stable: the loader's order within an instance is kept.
    std::ranges::stable_sort(entry.content.cards, [](const gi::Surfel& a, const gi::Surfel& b) {
        return a.instance_id < b.instance_id;
    });
    report.cells_loaded += 1;
    return cells_.push_back(std::move(entry));
}

void GiCellStreaming::release_cell(world::CellId cell, u64 frame,
                                   GiStreamingReport& report) noexcept {
    for (usize index = 0; index < cells_.size(); ++index) {
        Cell& entry = cells_[index];
        if (entry.id != cell) {
            continue;
        }
        for (const u64 chunk : entry.chunks) {
            system_->evict_cell(chunk, frame);
        }
        for (const u64 placement : entry.placements) {
            system_->field().remove(placement);
            report.fields_removed += 1;
        }
        for (const u32 instance : entry.instances) {
            system_->acceleration().remove_instance(instance);
            report.instances_removed += 1;
        }
        cards_in_gi_ -= entry.cards_in;
        report.cards_evicted += static_cast<u32>(entry.cards_in);
        const bool had_volumes = entry.complete && !entry.content.volumes.empty();
        cells_.erase(index);
        report.cells_evicted += 1;
        if (had_volumes) {
            report.volumes_changed = true;
        }
        return;
    }
}

Status GiCellStreaming::advance(Cell& cell, u64 frame, u32 cards_per_tick, u32& cards_left,
                                u32& fields_left, GiStreamingReport& report) noexcept {
    const Span<const gi::Surfel> cards = cell.content.cards.span();
    while (cell.next_card < cards.size() && cards_left > 0) {
        const usize end = chunk_end(cards, cell.next_card, cards_left);
        const auto count = static_cast<u32>(end - cell.next_card);
        if (count > cards_left && cards_left < cards_per_tick) {
            // One instance larger than what is left of this tick: it waits for a tick of its own.
            break;
        }
        const u64 chunk = kStreamedTag | next_id_++;
        const Span<const gi::Surfel> slice{cards.data() + cell.next_card, count};
        if (Status ingested = system_->ingest_cell(chunk, bounds_of(slice), slice, frame);
            !ingested) {
            return ingested;
        }
        if (Status kept = cell.chunks.push_back(chunk); !kept) {
            system_->evict_cell(chunk, frame);
            return kept;
        }
        cards_left = count >= cards_left ? 0U : cards_left - count;
        cell.next_card = end;
        cell.cards_in += count;
        cards_in_gi_ += count;
        report.chunks_ingested += 1;
        report.cards_ingested += count;
    }
    const Span<const GiFieldPlacement> fields = cell.content.fields.span();
    while (cell.next_field < fields.size() && fields_left > 0) {
        const GiFieldPlacement& placement = fields[cell.next_field];
        cell.next_field += 1;
        if (placement.field == nullptr) {
            continue;
        }
        const u64 id = kStreamedTag | next_id_++;
        if (Status placed = system_->field().place(id, *placement.field, placement.transform);
            !placed) {
            return placed;
        }
        if (Status kept = cell.placements.push_back(id); !kept) {
            system_->field().remove(id);
            return kept;
        }
        fields_left -= 1;
        report.fields_placed += 1;
    }
    // The hardware tier's instances go in with the fields, on the same budget: each is a structure
    // the service refits or builds on its next update.
    const Span<const rt::InstanceDescriptor> instances = cell.content.instances.span();
    while (cell.next_instance < instances.size() && fields_left > 0) {
        Expected<u32, Error> added =
            system_->acceleration().add_instance(instances[cell.next_instance]);
        if (!added) {
            return make_unexpected(added.error());
        }
        cell.next_instance += 1;
        if (Status kept = cell.instances.push_back(*added); !kept) {
            system_->acceleration().remove_instance(*added);
            return kept;
        }
        fields_left -= 1;
        report.instances_added += 1;
    }
    if (cell.next_card >= cards.size() && cell.next_field >= fields.size() &&
        cell.next_instance >= instances.size() && !cell.complete) {
        cell.complete = true;
        report.cells_completed += 1;
        if (!cell.content.volumes.empty()) {
            report.volumes_changed = true;
        }
    }
    return ok();
}

Status GiCellStreaming::place_volumes() noexcept {
    // The streamed cells' volumes as one owner's set: every complete cell's, in arrival order.
    Array<gi::GiVolume> volumes(*allocator_);
    for (const Cell& cell : cells_) {
        if (cell.complete) {
            if (Status appended = volumes.append(cell.content.volumes.span()); !appended) {
                return appended;
            }
        }
    }
    return system_->set_volumes(gi::GiVolumeOwner::Streaming, volumes.span());
}

Expected<GiStreamingReport, Error> GiCellStreaming::tick(u64 frame,
                                                         const GiStreamingBudget& budget) noexcept {
    GiStreamingReport report;
    if (events_ == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "GiCellStreaming: not attached"});
    }
    drained_.clear();
    if (Status drained = events_->drain(consumer_, drained_); !drained) {
        return make_unexpected(drained.error());
    }
    report.events_drained = static_cast<u32>(drained_.size());
    for (const world::CellEvent& event : drained_) {
        const bool lit = event.channels.has(world::Channel::Illumination);
        switch (event.kind) {
            case world::CellEventKind::Activated:
                if (!lit) {
                    report.cells_without_channel += 1;
                    break;
                }
                if (Status bound = bind_cell(event.cell, report); !bound) {
                    return make_unexpected(bound.error());
                }
                break;
            case world::CellEventKind::ChannelsChanged:
                if (!lit) {
                    release_cell(event.cell, frame, report);
                }
                break;
            case world::CellEventKind::Deactivated:
            case world::CellEventKind::Evicted:
                release_cell(event.cell, frame, report);
                break;
            case world::CellEventKind::Resident:
            case world::CellEventKind::Failed:
                break;
        }
    }

    // THE BUDGET, spent first on what left — a page released and a placement removed cost the
    // device copies and the field what one arriving does — and the rest on the cells in the order
    // they arrived. A tick that evicted a whole budget's worth brings nothing in.
    u32 cards_left = budget.cards_per_tick - std::min(budget.cards_per_tick, report.cards_evicted);
    u32 fields_left =
        budget.fields_per_tick - std::min(budget.fields_per_tick, report.fields_removed);
    for (Cell& cell : cells_) {
        if (cell.complete) {
            continue;
        }
        if (cards_left == 0 && fields_left == 0) {
            break;
        }
        if (Status advanced =
                advance(cell, frame, budget.cards_per_tick, cards_left, fields_left, report);
            !advanced) {
            return make_unexpected(advanced.error());
        }
    }
    for (const Cell& cell : cells_) {
        report.cells_pending += cell.complete ? 0U : 1U;
    }
    if (report.volumes_changed) {
        if (Status placed = place_volumes(); !placed) {
            return make_unexpected(placed.error());
        }
    }
    events_->compact();
    return report;
}

void GiCellStreaming::clear(u64 frame) noexcept {
    GiStreamingReport ignored;
    while (!cells_.empty()) {
        release_cell(cells_[cells_.size() - 1].id, frame, ignored);
    }
    (void)system_->set_volumes(gi::GiVolumeOwner::Streaming, {});
}

}  // namespace cy::rendering::gi_streaming
