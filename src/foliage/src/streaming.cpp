// Clusters bound to world cells, through the queue the world already emits. See streaming.h for why
// "evicted with them" falls out of consuming the same events.

#include <cy/foliage/streaming.h>

namespace cy::foliage {

FoliageStreaming::FoliageStreaming(Allocator& allocator, ClusterStore& clusters, GrassField& grass,
                                   const world::PartitionConfig& partition) noexcept
    : allocator_(&allocator),
      clusters_(&clusters),
      grass_(&grass),
      partition_(&partition),
      cells_(allocator),
      index_(allocator),
      drained_(allocator) {}

Status FoliageStreaming::attach(world::CellEventQueue& events, u32 order) noexcept {
    Expected<world::CellEventQueue::ConsumerId, Error> consumer =
        events.add_consumer("foliage.clusters", order);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    events_ = &events;
    consumer_ = consumer.value();
    attached_ = true;
    return ok();
}

FoliageStreaming::Binding* FoliageStreaming::find_binding(world::CellId cell) noexcept {
    usize* slot = index_.find(cell.value);
    return slot == nullptr ? nullptr : &cells_[*slot];
}

Span<const ClusterId> FoliageStreaming::clusters_of(world::CellId cell) const noexcept {
    const usize* slot = index_.find(cell.value);
    return slot == nullptr ? Span<const ClusterId>() : cells_[*slot].clusters.span();
}

Status FoliageStreaming::bind_cell(world::CellId cell, StreamingReport& report) noexcept {
    if (find_binding(cell) != nullptr) {
        return ok();  // Already bound; a `ChannelsChanged` on a cell that already has foliage.
    }
    Array<FoliagePopulationHandle> produced(*allocator_);
    if (loader_ != nullptr) {
        const Status loaded = loader_(loader_user_, cell, *partition_, produced);
        if (!loaded) {
            if (loaded.error().code == ErrorCode::NotFound) {
                // Most cells in a world have no foliage. Recording the binding with zero clusters
                // is what stops the streamer asking again every frame.
                ++report.cells_empty;
            } else {
                ++report.loader_failures;
                return ok();
            }
        }
    } else {
        ++report.cells_empty;
    }

    Binding binding(*allocator_);
    binding.cell = cell;
    for (FoliagePopulationHandle& handle : produced) {
        const ClusterId id = handle.cluster.id();
        const ClusterBounds bounds = handle.cluster.bounds();
        handle.cluster.set_cell(cell);
        if (Status inserted = clusters_->insert(static_cast<FoliageCluster&&>(handle.cluster));
            !inserted) {
            return inserted;
        }
        for (const GrassPatch& patch : handle.patches) {
            if (Status added = grass_->add(id, bounds, patch); !added) {
                return added;
            }
        }
        if (Status pushed = binding.clusters.push_back(id); !pushed) {
            return pushed;
        }
        ++report.clusters_loaded;
    }
    if (Status pushed = cells_.push_back(static_cast<Binding&&>(binding)); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> placed = index_.insert(cell.value, cells_.size() - 1); !placed) {
        cells_.pop_back();
        return fail(placed.error().code, placed.error().message);
    }
    ++report.cells_bound;
    return ok();
}

u32 FoliageStreaming::release_cell(world::CellId cell) noexcept {
    const usize* slot = index_.find(cell.value);
    if (slot == nullptr) {
        return 0;
    }
    const usize position = *slot;
    u32 evicted = 0;
    for (ClusterId cluster : cells_[position].clusters) {
        evicted += clusters_->evict(cluster) ? 1U : 0U;
        (void)grass_->evict(cluster);
    }
    const usize last = cells_.size() - 1;
    if (position != last) {
        cells_[position] = static_cast<Binding&&>(cells_[last]);
        if (usize* moved = index_.find(cells_[position].cell.value); moved != nullptr) {
            *moved = position;
        }
    }
    cells_.pop_back();
    (void)index_.remove(cell.value);
    return evicted;
}

Expected<StreamingReport, Error> FoliageStreaming::tick() noexcept {
    StreamingReport report;
    if (!attached_) {
        return fail(ErrorCode::Unavailable,
                    "FoliageStreaming::attach() has not been called: foliage consumes the world's "
                    "own cell events rather than observing cells, so there is nothing to tick");
    }
    drained_.clear();
    if (Status drained = events_->drain(consumer_, drained_); !drained) {
        return make_unexpected(drained.error());
    }
    report.events_drained = static_cast<u32>(drained_.size());
    for (const world::CellEvent& event : drained_) {
        switch (event.kind) {
            case world::CellEventKind::Resident:
            case world::CellEventKind::Activated:
            case world::CellEventKind::ChannelsChanged:
                if (!event.channels.has(world::Channel::Foliage)) {
                    // A server profile or a spectator source. Counted so a world that streams no
                    // foliage says so rather than looking broken.
                    ++report.cells_without_channel;
                    break;
                }
                if (Status bound = bind_cell(event.cell, report); !bound) {
                    return make_unexpected(bound.error());
                }
                break;
            case world::CellEventKind::Deactivated:
                break;  // Entities withdrew; the cell's payload is still resident.
            case world::CellEventKind::Evicted: {
                const u32 evicted = release_cell(event.cell);
                if (evicted > 0 || index_.find(event.cell.value) == nullptr) {
                    report.clusters_evicted += evicted;
                }
                ++report.cells_released;
                break;
            }
            case world::CellEventKind::Failed:
                break;
        }
    }
    return report;
}

bool FoliageStreaming::drop_cluster(ClusterId cluster) noexcept {
    // The "independently evictable" half: memory pressure takes one cluster out of a resident cell
    // without the cell going anywhere. The binding keeps the identity so the cell's release does
    // not double-evict, and `ClusterStore::evict()` answers false for a cluster already gone.
    (void)grass_->evict(cluster);
    return clusters_->evict(cluster);
}

}  // namespace cy::foliage
