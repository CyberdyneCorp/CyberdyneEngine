#include <cy/servers/render/virtual_texturing/system.h>

#include <algorithm>
#include <cmath>

namespace cy::render::vt {

namespace {

/// How many finished results `collect_production` takes in one pass. Bounded so that a frame which
/// produced thousands of pages does not spend all of itself publishing them; the rest arrive next
/// frame, which is what the mip tail makes acceptable.
constexpr u32 kCollectBatch = 256;

/// A cooked tile is streamed; a runtime page is composed. The mapping is here rather than on
/// `ResidencyModel` because it is a statement about what producing a page COSTS, which is the
/// residency layer's vocabulary and not the address space's.
[[nodiscard]] residency::CostClass cost_class_of(ResidencyModel model) noexcept {
    return (model == ResidencyModel::VirtualRuntime) ? residency::CostClass::Composed
                                                     : residency::CostClass::Streamed;
}

/// Feedback importance is derived from the sample count: a page a million pixels asked for matters
/// more than one a hundred did. Normalised logarithmically, because the interesting range spans
/// four orders of magnitude and a linear map would put everything but the sky at zero.
[[nodiscard]] f32 importance_from_samples(u32 samples) noexcept {
    if (samples == 0) {
        return 0.0F;
    }
    const f32 scaled = std::log2(1.0F + static_cast<f32>(samples)) / 20.0F;  // 2^20 ≈ a full screen
    return (scaled > 1.0F) ? 1.0F : scaled;
}

/// Visit every page of `desc` at `mip`, over every layer, in a fixed order. Two callers walk the
/// same pyramid — pinning the mip tail and prefetching coarse levels — and writing the four nested
/// loops twice is how the two quietly stop agreeing about the order they visit pages in.
template <class Visit>
[[nodiscard]] Status for_each_page(const VirtualTextureDesc& desc, u8 mip, Visit&& visit) noexcept {
    for (u8 layer = 0; layer < desc.layers; ++layer) {
        for (u32 y = 0; y < desc.tiles_y(mip); ++y) {
            for (u32 x = 0; x < desc.tiles_x(mip); ++x) {
                VirtualAddress address;
                address.texture = desc.id;
                address.mip = mip;
                address.layer = layer;
                address.tile_x = static_cast<u16>(x);
                address.tile_y = static_cast<u16>(y);
                if (Status visited = visit(address); !visited) {
                    return visited;
                }
            }
        }
    }
    return ok();
}

/// Take a pinned tile for one mip tail page and publish it in the page table.
[[nodiscard]] Status pin_tail_page(PageTable& table, PhysicalTileCache& tile_cache,
                                   const VirtualAddress& address) noexcept {
    auto tile = tile_cache.acquire(address.encode(), true);
    if (!tile) {
        // THE TAIL IS THE GUARANTEE. A cache too small to hold it is a configuration error and must
        // be reported as one — the alternative is a system that runs and shows holes.
        return make_unexpected(
            Error{ErrorCode::OutOfMemory,
                  "virtual texturing: the physical cache cannot hold this texture's mip tail, so a "
                  "frame could be missing rather than blurry; raise the cache budget or shorten "
                  "the tail"});
    }
    PageTableEntry entry;
    entry.physical_tile = *tile;
    entry.resident_mip = address.mip;
    entry.flags = PageFlags::kResident | PageFlags::kPinned;
    return table.stage(address, entry);
}

}  // namespace

// --- ProductionPool
// ----------------------------------------------------------------------------------

ProductionPool::~ProductionPool() {
    stop();
}

Status ProductionPool::start(u32 workers) noexcept {
    if (workers == 0 || workers > kMaxWorkers) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: production worker count must be between "
                                     "1 and ProductionPool::kMaxWorkers"});
    }
    stop();
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = false;
        head_ = 0;
        queue_.clear();
        done_.clear();
    }
    for (u32 index = 0; index < workers; ++index) {
        threads_[index] = std::thread([this]() noexcept { worker_loop(); });
    }
    {
        // Published under the lock, because `submit` reads it to decide whether to run the job here
        // or queue it, and an unsynchronised write would be a race between a caller and a start.
        const std::lock_guard<std::mutex> guard(mutex_);
        worker_count_ = workers;
    }
    return ok();
}

void ProductionPool::stop() noexcept {
    u32 joining = 0;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_ && worker_count_ == 0) {
            return;
        }
        stopping_ = true;
        joining = worker_count_;
    }
    wake_.notify_all();
    for (u32 index = 0; index < joining; ++index) {
        if (threads_[index].joinable()) {
            threads_[index].join();
        }
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    worker_count_ = 0;
}

void ProductionPool::quiesce() noexcept {
    std::unique_lock<std::mutex> guard(mutex_);
    idle_.wait(guard, [this]() noexcept { return (queue_.size() - head_) == 0 && running_ == 0; });
}

Status ProductionPool::submit(const ProductionJob& job) noexcept {
    if (job.producer == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "virtual texturing: a production job with no producer"});
    }
    bool inline_here = false;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            return make_unexpected(Error{ErrorCode::Unavailable,
                                         "virtual texturing: production is stopping; the job was "
                                         "refused rather than queued against a pool that is going "
                                         "away"});
        }
        // NO WORKERS IS A SUPPORTED CONFIGURATION, not an error, and the job then runs on the
        // caller's thread. A cook, a test and a headless tool all want production without a pool,
        // and the alternative — queueing against nothing — is a `quiesce()` that never returns.
        // CyberInput makes the same choice about a device backend, for the same reason: the absence
        // should be the simple case rather than a mode.
        inline_here = worker_count_ == 0;
        if (!inline_here) {
            if (Status pushed = queue_.push_back(job); !pushed) {
                return pushed;
            }
        } else {
            ++running_;
        }
    }
    if (!inline_here) {
        wake_.notify_one();
        return ok();
    }

    // Outside the lock, so that a producer which touches the pool — invalidating a page it just
    // learned is stale, say — does not deadlock against the caller that dispatched it.
    const Status produced = job.producer->produce(job.request);
    Status recorded = ok();
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        ProductionResult result;
        result.address = job.request.address;
        result.physical_tile = job.request.physical_tile;
        result.succeeded = produced.has_value();
        if (result.succeeded) {
            ++produced_;
        } else {
            ++failed_;
        }
        recorded = done_.push_back(result);
        --running_;
    }
    idle_.notify_all();
    return recorded;
}

void ProductionPool::worker_loop() noexcept {
    while (true) {
        ProductionJob job;
        {
            std::unique_lock<std::mutex> guard(mutex_);
            wake_.wait(guard,
                       [this]() noexcept { return stopping_ || (queue_.size() - head_) != 0; });
            if (stopping_) {
                return;
            }
            job = queue_[head_];
            ++head_;
            if (head_ == queue_.size()) {
                queue_.clear();
                head_ = 0;
            }
            ++running_;
        }

        const Status produced = job.producer->produce(job.request);

        {
            const std::lock_guard<std::mutex> guard(mutex_);
            ProductionResult result;
            result.address = job.request.address;
            result.physical_tile = job.request.physical_tile;
            result.succeeded = produced.has_value();
            if (result.succeeded) {
                ++produced_;
            } else {
                ++failed_;
            }
            if (Status pushed = done_.push_back(result); !pushed) {
                // The page was produced; only the report of it was lost. The page table entry stays
                // pending and the tile stays reserved until the next eviction, which is a leak of
                // one tile rather than a corrupted cache.
                ++failed_;
            }
            --running_;
        }
        idle_.notify_all();
    }
}

u32 ProductionPool::drain(ProductionResult* out, u32 capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const u32 available = static_cast<u32>(std::min<usize>(done_.size(), capacity));
    for (u32 index = 0; index < available; ++index) {
        out[index] = done_[index];
    }
    // Shift the remainder down. `done_` holds at most one frame's completions, so this is a memmove
    // over a few hundred entries rather than a structure worth optimising.
    const usize remaining = done_.size() - available;
    for (usize index = 0; index < remaining; ++index) {
        done_[index] = done_[index + available];
    }
    while (done_.size() > remaining) {
        done_.pop_back();
    }
    return available;
}

u32 ProductionPool::workers() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return worker_count_;
}

usize ProductionPool::pending() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return (queue_.size() - head_) + running_;
}

u64 ProductionPool::produced() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return produced_;
}

u64 ProductionPool::failed() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return failed_;
}

// --- VirtualTextureSystem
// -----------------------------------------------------------------------------

VirtualTextureSystem::VirtualTextureSystem(Allocator& allocator) noexcept
    : allocator_(&allocator),
      textures_(allocator),
      texture_slots_(allocator),
      caches_{PhysicalTileCache(allocator), PhysicalTileCache(allocator),
              PhysicalTileCache(allocator), PhysicalTileCache(allocator)},
      producers_(allocator),
      feedback_(allocator),
      resolved_(allocator),
      pool_(allocator) {}

VirtualTextureSystem::~VirtualTextureSystem() {
    // `pool_` is the last member and is destroyed first, joining every worker before a cache or a
    // page table is touched. The explicit stop is belt and braces for a reader who reorders the
    // members without reading the note in the header.
    pool_.stop();
}

Status VirtualTextureSystem::configure_cache(const TileCacheDesc& desc) noexcept {
    if (static_cast<u32>(desc.format) >= kFormatClassCount) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "virtual texturing: unknown format class"});
    }
    pool_.quiesce();
    return caches_[static_cast<u32>(desc.format)].configure(desc);
}

PhysicalTileCache& VirtualTextureSystem::cache(FormatClass klass) noexcept {
    const u32 index = static_cast<u32>(klass);
    return caches_[(index < kFormatClassCount) ? index : 0];
}

const PhysicalTileCache& VirtualTextureSystem::cache(FormatClass klass) const noexcept {
    const u32 index = static_cast<u32>(klass);
    return caches_[(index < kFormatClassCount) ? index : 0];
}

Status VirtualTextureSystem::start_production(u32 workers) noexcept {
    return pool_.start(workers);
}

Status VirtualTextureSystem::register_texture(const VirtualTextureDesc& desc) noexcept {
    if (Status valid = validate_description(desc); !valid) {
        return valid;
    }
    if (texture_slots_.contains(desc.id)) {
        return make_unexpected(Error{ErrorCode::AlreadyExists,
                                     "virtual texturing: a texture with this id is registered"});
    }
    TextureRecord record(*allocator_);
    record.desc = desc;
    if (Status configured = record.table.configure(desc); !configured) {
        return configured;
    }
    if (Status pushed = textures_.push_back(std::move(record)); !pushed) {
        return pushed;
    }
    if (auto placed = texture_slots_.insert(desc.id, textures_.size() - 1); !placed) {
        textures_.pop_back();
        return make_unexpected(placed.error());
    }
    return ok();
}

bool VirtualTextureSystem::unregister_texture(u32 texture) noexcept {
    const usize* slot = texture_slots_.find(texture);
    if (slot == nullptr) {
        return false;
    }
    // QUIESCE BEFORE FREEING. A running producer holds a pointer into this texture's tiles; the
    // pool must be idle before the cache slots are released and the page table goes away.
    pool_.quiesce();

    const usize index = *slot;
    const VirtualTextureDesc desc = textures_[index].desc;
    PhysicalTileCache& tile_cache = cache(desc.format_class());
    for (u32 tile = 0; tile < tile_cache.capacity(); ++tile) {
        const TileSlot* occupied = tile_cache.slot(tile);
        if (occupied == nullptr || !occupied->occupied) {
            continue;
        }
        if (VirtualAddress::decode(occupied->address).texture != texture) {
            continue;
        }
        tile_cache.set_pinned(tile, false);
        tile_cache.release(tile);
    }

    texture_slots_.remove(texture);
    const usize last = textures_.size() - 1;
    if (index != last) {
        textures_[index] = std::move(textures_[last]);
        if (usize* moved = texture_slots_.find(textures_[index].desc.id); moved != nullptr) {
            *moved = index;
        }
    }
    textures_.pop_back();
    producers_.unregister_producer(texture);
    return true;
}

VirtualTextureSystem::TextureRecord* VirtualTextureSystem::find_texture(u32 texture) noexcept {
    const usize* slot = texture_slots_.find(texture);
    return (slot == nullptr) ? nullptr : &textures_[*slot];
}

const VirtualTextureSystem::TextureRecord* VirtualTextureSystem::find_texture(
    u32 texture) const noexcept {
    const usize* slot = texture_slots_.find(texture);
    return (slot == nullptr) ? nullptr : &textures_[*slot];
}

const VirtualTextureDesc* VirtualTextureSystem::description(u32 texture) const noexcept {
    const TextureRecord* record = find_texture(texture);
    return (record == nullptr) ? nullptr : &record->desc;
}

const PageTable* VirtualTextureSystem::page_table(u32 texture) const noexcept {
    const TextureRecord* record = find_texture(texture);
    return (record == nullptr) ? nullptr : &record->table;
}

Status VirtualTextureSystem::make_mip_tail_resident(u32 texture) noexcept {
    TextureRecord* record = find_texture(texture);
    if (record == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "virtual texturing: no such virtual texture"});
    }
    PhysicalTileCache& tile_cache = cache(record->desc.format_class());
    if (!tile_cache.configured()) {
        return make_unexpected(Error{ErrorCode::Unavailable,
                                     "virtual texturing: the cache for this format class has not "
                                     "been configured, so the mip tail has nowhere to live"});
    }

    const VirtualTextureDesc desc = record->desc;
    for (u8 mip = desc.mip_tail_base(); mip < desc.mip_count; ++mip) {
        const Status pinned = for_each_page(desc, mip, [&](const VirtualAddress& address) noexcept {
            return pin_tail_page(record->table, tile_cache, address);
        });
        if (!pinned) {
            return pinned;
        }
    }
    record->table.apply_staged();
    record->tail_resident = true;
    return ok();
}

bool VirtualTextureSystem::mip_tail_resident(u32 texture) const noexcept {
    const TextureRecord* record = find_texture(texture);
    return (record != nullptr) && record->tail_resident;
}

SampleResult VirtualTextureSystem::sample(const VirtualAddress& wanted) const noexcept {
    SampleResult result;
    result.resolved = wanted;
    result.desired_mip = wanted.mip;

    const TextureRecord* record = find_texture(wanted.texture);
    if (record == nullptr) {
        return result;
    }

    // THE WALK. From the level that was asked for towards the coarsest, stopping at the first
    // resident entry. Because the mip tail is pinned and cannot be evicted, this terminates on a
    // resident level for every address of a texture whose tail was made resident — which is the
    // whole of "a surface is never missing, only blurry".
    for (u8 mip = wanted.mip; mip < record->desc.mip_count; ++mip) {
        const u32 shift = mip - wanted.mip;
        VirtualAddress probe;
        probe.texture = wanted.texture;
        probe.layer = wanted.layer;
        probe.mip = mip;
        probe.tile_x = static_cast<u16>(wanted.tile_x >> shift);
        probe.tile_y = static_cast<u16>(wanted.tile_y >> shift);

        const PageTableEntry entry = record->table.lookup(probe);
        if (!entry.resident()) {
            continue;
        }
        result.resolved = probe;
        result.physical_tile = entry.physical_tile;
        result.resident_mip = mip;
        result.deficit = static_cast<u8>(shift);
        result.fallback = shift != 0;
        result.missing = false;
        return result;
    }
    return result;
}

SampleResult VirtualTextureSystem::sample_and_count(const VirtualAddress& wanted) noexcept {
    const SampleResult result = sample(wanted);
    ++stats_.samples;
    stats_.fallback_samples += result.fallback ? 1U : 0U;
    stats_.missing_samples += result.missing ? 1U : 0U;
    return result;
}

Status VirtualTextureSystem::request_page(residency::ResidencyServer& server,
                                          const VirtualAddress& page, u32 samples, f32 confidence,
                                          f64 seconds_until, f64 now) noexcept {
    (void)now;
    const TextureRecord* record = find_texture(page.texture);
    if (record == nullptr) {
        return ok();  // feedback for a texture that has since been unregistered is not an error
    }
    const SampleResult current = sample(page);

    residency::Request request;
    request.key = page_key(page.encode());
    request.bytes = record->desc.bytes_per_tile;
    request.guaranteed = record->desc.in_mip_tail(page.mip);
    request.instance = page.texture;
    request.inputs.importance = importance_from_samples(samples);
    request.inputs.screen_coverage = importance_from_samples(samples);
    request.inputs.detail_deficit = current.missing ? record->desc.mip_count : current.deficit;
    request.inputs.prediction_confidence = confidence;
    request.inputs.seconds_until_needed = seconds_until;
    request.inputs.cost = cost_class_of(record->desc.model);
    if (const PageProducer* producer = producers_.find(page.texture); producer != nullptr) {
        request.inputs.production_cost_ms = producer->declared_cost_ms();
    }
    ++stats_.requests_submitted;
    return server.request(request);
}

Status VirtualTextureSystem::submit_feedback_requests(residency::ResidencyServer& server,
                                                      f64 now) noexcept {
    feedback_.swap();
    if (Status resolved = feedback_.resolve(resolved_); !resolved) {
        return resolved;
    }
    stats_.feedback_requests += resolved_.size();
    for (const FeedbackRequest& entry : resolved_) {
        const VirtualAddress address = VirtualAddress::decode(entry.address);
        // Confidence is 1: feedback is not a prediction, it is a measurement of what was sampled.
        if (Status requested =
                request_page(server, address, entry.samples, 1.0F, residency::kNoDeadline, now);
            !requested) {
            return requested;
        }
    }
    return ok();
}

Status VirtualTextureSystem::prefetch(residency::ResidencyServer& server, u32 texture,
                                      u8 coarsest_wanted, f64 seconds_until,
                                      residency::PredictionSource source, f64 now) noexcept {
    const TextureRecord* record = find_texture(texture);
    if (record == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "virtual texturing: prefetch for no such texture"});
    }
    const VirtualTextureDesc desc = record->desc;

    // COARSE PAGES ONLY. "prediction covers latency, feedback establishes accuracy. Neither SHALL
    // be relied on for the other's role." Prefetching the fine levels would be guessing at what
    // will be sampled, and the pages that turned out wrong would evict the ones that were right.
    const u8 below_tail =
        (desc.mip_tail_base() > 0) ? static_cast<u8>(desc.mip_tail_base() - 1) : 0U;
    u8 finest = (coarsest_wanted > below_tail) ? coarsest_wanted : below_tail;
    finest = (finest > desc.coarsest_mip()) ? desc.coarsest_mip() : finest;

    u64 requested = 0;
    for (u8 mip = desc.coarsest_mip();; --mip) {
        const Status asked = for_each_page(desc, mip, [&](const VirtualAddress& address) noexcept {
            ++requested;
            return request_page(server, address, 1, 0.5F, seconds_until, now);
        });
        if (!asked) {
            return asked;
        }
        if (mip == finest) {
            break;
        }
    }
    stats_.prefetched_pages += requested;
    server.record_prefetched(source, requested);
    return ok();
}

Status VirtualTextureSystem::apply_admission(const residency::Admission& admission,
                                             residency::ResidencyServer& server) noexcept {
    const VirtualAddress address = VirtualAddress::decode(admission.key.page);
    TextureRecord* record = find_texture(address.texture);
    if (record == nullptr) {
        server.cancel_admission(admission.key);
        return ok();
    }
    PhysicalTileCache& tile_cache = cache(record->desc.format_class());
    auto tile = tile_cache.acquire(address.encode(), false);
    if (!tile) {
        // The cache is full. Not an error: the residency policy orders an eviction on a later
        // frame, and until then the surface renders from a coarser resident level. The admission
        // is handed back so the policy stops counting bytes for a page that will not arrive.
        ++stats_.admissions_refused;
        server.cancel_admission(admission.key);
        return ok();
    }

    PageTableEntry pending;
    pending.physical_tile = *tile;
    pending.resident_mip = kNoResidentMip;
    pending.flags = PageFlags::kPending;
    if (Status staged = record->table.stage(address, pending); !staged) {
        tile_cache.release(*tile);
        server.cancel_admission(admission.key);
        return staged;
    }

    PageProducer* producer = producers_.find(address.texture);
    if (producer == nullptr) {
        tile_cache.release(*tile);
        server.cancel_admission(admission.key);
        return make_unexpected(Error{ErrorCode::Unavailable,
                                     "virtual texturing: no producer is registered for this "
                                     "texture, so its pages cannot be filled — the cooked disk "
                                     "tile reader is a producer like any other"});
    }

    ProductionJob job;
    job.producer = producer;
    job.request.address = address;
    job.request.physical_tile = *tile;
    job.request.destination = tile_cache.tile_data(*tile);
    job.request.bytes = tile_cache.description().bytes_per_tile;
    if (Status submitted = pool_.submit(job); !submitted) {
        tile_cache.release(*tile);
        server.cancel_admission(admission.key);
        return submitted;
    }
    ++stats_.admissions_applied;
    return ok();
}

void VirtualTextureSystem::release_tile(const VirtualAddress& address) noexcept {
    TextureRecord* record = find_texture(address.texture);
    if (record == nullptr) {
        return;
    }
    PhysicalTileCache& tile_cache = cache(record->desc.format_class());
    tile_cache.release_address(address.encode());

    PageTableEntry cleared;
    cleared.flags = PageFlags::kInvalid;
    if (Status staged = record->table.stage(address, cleared); !staged) {
        // Nothing points at the tile any more either way; the entry is corrected on the next stage
        // for this page, and until then a sample walks to a coarser level.
    }
}

void VirtualTextureSystem::apply_eviction(const residency::EvictionOrder& order) noexcept {
    release_tile(VirtualAddress::decode(order.key.page));
    ++stats_.evictions_applied;
}

Status VirtualTextureSystem::apply(const residency::Schedule& schedule,
                                   residency::ResidencyServer& server, f64 now) noexcept {
    (void)now;
    for (const residency::EvictionOrder& order : schedule.evictions) {
        if (order.key.subsystem == kResidencySubsystem) {
            apply_eviction(order);
        }
    }
    for (const residency::Admission& admission : schedule.admissions) {
        if (admission.key.subsystem != kResidencySubsystem) {
            continue;
        }
        if (Status applied = apply_admission(admission, server); !applied) {
            return applied;
        }
    }
    return ok();
}

u32 VirtualTextureSystem::collect_production(residency::ResidencyServer& server, f64 now) noexcept {
    ProductionResult results[kCollectBatch];
    const u32 count = pool_.drain(results, kCollectBatch);
    u32 published = 0;
    for (u32 index = 0; index < count; ++index) {
        const ProductionResult& result = results[index];
        TextureRecord* record = find_texture(result.address.texture);
        if (record == nullptr) {
            continue;  // the texture went away while the page was being produced
        }
        if (!result.succeeded) {
            ++stats_.production_failures;
            release_tile(result.address);
            server.cancel_admission(page_key(result.address.encode()));
            continue;
        }

        PageTableEntry entry;
        entry.physical_tile = result.physical_tile;
        entry.resident_mip = result.address.mip;
        entry.flags = PageFlags::kResident;
        if (producers_.find(result.address.texture) != nullptr &&
            record->desc.model == ResidencyModel::VirtualRuntime) {
            entry.flags |= PageFlags::kRuntimeProduced;
        }
        if (Status staged = record->table.stage(result.address, entry); !staged) {
            continue;
        }

        residency::ResidentReport report;
        report.key = page_key(result.address.encode());
        report.bytes = record->desc.bytes_per_tile;
        report.level = result.address.mip;
        report.guaranteed = record->desc.in_mip_tail(result.address.mip);
        if (const PageProducer* producer = producers_.find(result.address.texture);
            producer != nullptr) {
            report.production_cost_ms = producer->declared_cost_ms();
        }
        report.cost = cost_class_of(record->desc.model);
        if (Status noted = server.note_resident(report, now); !noted) {
            continue;
        }
        ++stats_.pages_produced;
        ++published;
    }
    return published;
}

void VirtualTextureSystem::end_frame() noexcept {
    for (TextureRecord& record : textures_) {
        const usize applied = record.table.apply_staged();
        stats_.page_table_updates += applied;
        if (applied != 0) {
            ++stats_.page_table_batches;
        }
    }
}

Status VirtualTextureSystem::invalidate(const VirtualAddress& address, u8 finer_levels) noexcept {
    TextureRecord* record = find_texture(address.texture);
    if (record == nullptr) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "virtual texturing: invalidating no such texture"});
    }
    // Quiesce first: a producer may be filling one of the tiles about to be released.
    pool_.quiesce();
    if (Status invalidated = record->table.invalidate(address, finer_levels); !invalidated) {
        return invalidated;
    }
    const PageTableUpdate* staged = record->table.staged();
    const usize staged_count = record->table.staged_count();
    PhysicalTileCache& tile_cache = cache(record->desc.format_class());
    for (usize index = 0; index < staged_count; ++index) {
        const VirtualAddress cleared = VirtualAddress::decode(staged[index].address);
        if (!record->desc.in_mip_tail(cleared.mip)) {
            // The mip tail is skipped deliberately: it is pinned, `release` would refuse it, and a
            // producer invalidating its inputs must not be able to take away the guarantee that a
            // surface is never missing.
            tile_cache.release_address(staged[index].address);
        }
    }
    record->table.apply_staged();
    return ok();
}

void VirtualTextureSystem::reset() noexcept {
    pool_.quiesce();
    textures_.clear();
    texture_slots_.clear();
    producers_.clear();
    feedback_.reset();
    resolved_.clear();
    for (PhysicalTileCache& tile_cache : caches_) {
        const TileCacheDesc desc = tile_cache.description();
        tile_cache.clear();
        if (desc.tile_capacity != 0 && desc.bytes_per_tile != 0) {
            if (Status configured = tile_cache.configure(desc); !configured) {
                // A reconfigure that cannot allocate leaves the cache cleared and unconfigured,
                // which `acquire` reports rather than crashing on.
            }
        }
    }
    stats_ = VirtualTextureStats{};
}

VirtualTextureStats VirtualTextureSystem::stats() const noexcept {
    VirtualTextureStats result = stats_;
    result.textures = textures_.size();
    result.feedback_recorded = feedback_.recorded();
    result.feedback_dropped = feedback_.dropped();
    for (const PhysicalTileCache& tile_cache : caches_) {
        result.resident_tiles += tile_cache.occupancy();
        result.pinned_tiles += tile_cache.pinned_count();
        result.cache_bytes += tile_cache.bytes_in_use();
        result.cache_budget += tile_cache.budget_bytes();
    }
    return result;
}

}  // namespace cy::render::vt
