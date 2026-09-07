// Partial residency over a budget. M7 tasks 2.1 and 2.2; see streaming.h for the argument.

#include <cy/core/assets/streaming.h>

#include <cy/core/memory/scope.h>

#include <thread>

namespace cy::assets {
namespace {

/// The clock value that sorts before every real request, so a level nothing has asked for since it
/// was admitted is the first thing evicted.
constexpr i64 kNeverRequested = 0;

}  // namespace

const char* stream_kind_name(StreamKind kind) noexcept {
    switch (kind) {
        case StreamKind::TextureMip:
            return "texture-mip";
        case StreamKind::MeshLod:
            return "mesh-lod";
        case StreamKind::AudioChunk:
            return "audio-chunk";
    }
    return "unknown";
}

StreamingSystem::StreamingSystem() noexcept = default;

StreamingSystem::~StreamingSystem() {
    shutdown();
}

Status StreamingSystem::start(jobs::AsyncService& async, VirtualFileSystem& files,
                              u64 budget_bytes) noexcept {
    if (running_) {
        return fail(ErrorCode::AlreadyExists, "the streaming system is already running");
    }
    if (budget_bytes == 0) {
        // Refused rather than treated as "unlimited". A zero budget would evict every level above
        // the base on the first update and report nothing but churn, and a caller that meant
        // "everything resident" has a number for that.
        return fail(ErrorCode::InvalidArgument, "a residency budget of zero streams nothing");
    }
    async_ = &async;
    files_ = &files;
    budget_bytes_ = budget_bytes;
    running_ = true;
    return ok();
}

void StreamingSystem::shutdown() noexcept {
    // WAIT FOR THE SERVICE THREAD BEFORE FREEING ANYTHING. A read in flight owns a pointer into a
    // `Read` cell this function is about to destroy; M5.5's gate found the same shape in the Jolt
    // job bridge and it reproduced one run in forty. Yielding rather than sleeping because the
    // outstanding work is a file read that is already running.
    while (reads_in_flight_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    reads_.clear();
    entries_.clear();
    resident_bytes_total_ = 0;
    stats_.resident_bytes = 0;
    stats_.resident_levels = 0;
    async_ = nullptr;
    files_ = nullptr;
    running_ = false;
}

StreamingSystem::Entry* StreamingSystem::find(cy::AssetId id, VariantKey variant) noexcept {
    for (UniquePtr<Entry>& entry : entries_) {
        if (entry->id == id && entry->variant == variant) {
            return entry.get();
        }
    }
    return nullptr;
}

const StreamingSystem::Entry* StreamingSystem::find(cy::AssetId id,
                                                    VariantKey variant) const noexcept {
    for (const UniquePtr<Entry>& entry : entries_) {
        if (entry->id == id && entry->variant == variant) {
            return entry.get();
        }
    }
    return nullptr;
}

Status StreamingSystem::declare(const StreamDeclaration& declaration) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the streaming system is not running");
    }
    if (declaration.levels.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "a streamed asset needs at least one level, because level 0 is what every "
                    "other level falls back to");
    }

    for (const StreamLevel& level : declaration.levels) {
        if (level.bytes == 0) {
            return fail(ErrorCode::InvalidArgument, "a declared level of zero bytes");
        }
    }
    if (declaration.levels[0].bytes > budget_bytes_) {
        // Named here, while the caller can still act. An asset whose BASE does not fit would be
        // admitted and evicted on alternating updates for the life of the process.
        return fail(ErrorCode::OutOfRange,
                    "a streamed asset whose base level alone exceeds the residency budget");
    }

    // Re-declaring replaces: a re-cooked asset's offsets have moved, so what was resident is
    // wrong rather than stale.
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index]->id != declaration.id ||
            entries_[index]->variant != declaration.variant) {
            continue;
        }
        for (const Level& level : entries_[index]->levels) {
            if (level.resident) {
                resident_bytes_total_ -= level.declared.bytes;
            }
        }
        entries_.erase(index);
        break;
    }

    Expected<UniquePtr<Entry>, Error> created =
        make_unique<Entry>(default_allocator(), default_allocator());
    if (!created) {
        return make_unexpected(created.error());
    }
    Entry& entry = *created.value();
    entry.id = declaration.id;
    entry.variant = declaration.variant;
    entry.kind = declaration.kind;
    entry.source = declaration.source;
    if (Status reserved = entry.levels.reserve(declaration.levels.size()); !reserved) {
        return reserved;
    }
    for (const StreamLevel& declared : declaration.levels) {
        Level level;
        level.declared = declared;
        level.last_request_ns = kNeverRequested;
        if (Status added = entry.levels.push_back(std::move(level)); !added) {
            return added;
        }
    }

    if (Status stored = entries_.push_back(std::move(created.value())); !stored) {
        return stored;
    }
    ++stats_.declared;
    return ok();
}

Status StreamingSystem::request(cy::AssetId id, VariantKey variant, u32 level,
                                i64 now_ns) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the streaming system is not running");
    }
    Entry* entry = find(id, variant);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no streaming ladder was declared for this asset");
    }
    // A request above the top of the ladder is CLAMPED rather than refused. Feedback derived from
    // a screen-space error is a continuous quantity meeting a discrete ladder, and "closer than the
    // finest level" is an ordinary thing for it to say.
    const u32 wanted =
        level >= entry->levels.size() ? static_cast<u32>(entry->levels.size() - 1) : level;

    ++stats_.requests;
    // Every level up to and including the wanted one is refreshed: a fallback that is still being
    // drawn from is still in use, and evicting it because the frame asked for something finer is
    // how a stall gets designed in.
    for (u32 index = 0; index <= wanted; ++index) {
        entry->levels[index].last_request_ns = now_ns;
    }
    entry->wanted = wanted;
    entry->wanted_at_ns = now_ns;
    if (entry->levels[wanted].resident) {
        ++stats_.requests_satisfied;
    }
    return ok();
}

u32 StreamingSystem::resident_level(cy::AssetId id, VariantKey variant) const noexcept {
    const Entry* entry = find(id, variant);
    if (entry == nullptr) {
        return kInvalidLevel;
    }
    u32 best = kInvalidLevel;
    for (u32 index = 0; index < entry->levels.size(); ++index) {
        if (entry->levels[index].resident) {
            best = index;
        }
    }
    return best;
}

Span<const u8> StreamingSystem::resident_bytes(cy::AssetId id, VariantKey variant) const noexcept {
    const u32 level = resident_level(id, variant);
    if (level == kInvalidLevel) {
        return {};
    }
    return level_bytes(id, variant, level);
}

Span<const u8> StreamingSystem::level_bytes(cy::AssetId id, VariantKey variant,
                                            u32 level) const noexcept {
    const Entry* entry = find(id, variant);
    if (entry == nullptr || level >= entry->levels.size() || !entry->levels[level].resident) {
        return {};
    }
    const Array<u8>& bytes = entry->levels[level].bytes;
    return {bytes.data(), bytes.size()};
}

void StreamingSystem::perform_read(void* user) noexcept {
    Read& read = *static_cast<Read*>(user);
    // THE OWNER IS TAKEN INTO A LOCAL FIRST, AND THE `finished` STORE IS THE LAST THING THAT
    // TOUCHES THE CELL. That ordering is the whole of this function's correctness, and the first
    // draft got it wrong: it published `finished` and THEN read `read.owner` to decrement the
    // in-flight count, so `collect_finished` on the frame thread could free the cell in between.
    // ThreadSanitizer reported it six times over eight test cases —
    //
    //     WARNING: ThreadSanitizer: data race
    //       Write of size 8 by main thread: ... StreamingSystem::collect_finished()
    //       Previous read of size 8 by thread T5: ... StreamingSystem::perform_read
    //
    // — which is the same shape M5.5's gate found in the Jolt job bridge, and it would have
    // reproduced in a shipped frame loop rather than in a test.
    StreamingSystem* const owner = read.owner;

    // The one thread on which blocking is legal, which is the whole reason this is here and not in
    // a job body. `core-jobs-and-concurrency` refuses and counts a blocking region on a worker.
    const Status status = owner->files_->read_range(
        read.source, read.offset, static_cast<void*>(read.bytes.data()), read.size);
    read.ok = static_cast<bool>(status);
    // Release: everything written into the cell above happens-before the acquire in
    // `collect_finished`. Nothing may touch `read` after this line.
    read.finished.store(true, std::memory_order_release);

    // `owner` outlives every read by construction — `shutdown` waits for this counter to reach zero
    // before it frees anything, and the destructor calls `shutdown`.
    owner->reads_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

void StreamingSystem::collect_finished() noexcept {
    usize index = 0;
    while (index < reads_.size()) {
        Read& read = *reads_[index];
        if (!read.finished.load(std::memory_order_acquire)) {
            ++index;
            continue;
        }
        Entry* entry = find(read.id, read.variant);
        if (entry != nullptr && read.level < entry->levels.size()) {
            Level& level = entry->levels[read.level];
            level.in_flight = false;
            if (read.ok) {
                level.bytes = std::move(read.bytes);
                level.resident = true;
                resident_bytes_total_ += level.declared.bytes;
                ++stats_.levels_loaded;
                stats_.bytes_loaded += level.declared.bytes;
            } else {
                // The level stays absent and the asset keeps the level below it. A failed read is
                // not a frame's problem; it is a diagnostic's.
                ++stats_.load_failures;
            }
        }
        reads_.erase(index);
    }
}

u64 StreamingSystem::in_flight_bytes() const noexcept {
    u64 total = 0;
    for (const UniquePtr<Read>& read : reads_) {
        total += read->size;
    }
    return total;
}

void StreamingSystem::admit(i64 now_ns) noexcept {
    bool shortfall = false;
    for (const UniquePtr<Entry>& held : entries_) {
        Entry& entry = *held;
        // Coarsest-first: the base is what the fallback guarantee rests on, so it is admitted
        // before anything finer even when the frame asked only for the finest.
        for (u32 level_index = 0; level_index <= entry.wanted; ++level_index) {
            Level& level = entry.levels[level_index];
            if (level.resident || level.in_flight) {
                continue;
            }
            const u64 committed = resident_bytes_total_ + in_flight_bytes();
            if (committed + level.declared.bytes > budget_bytes_) {
                ++stats_.requests_deferred;
                shortfall = true;
                continue;
            }

            Expected<UniquePtr<Read>, Error> created =
                make_unique<Read>(default_allocator(), default_allocator());
            if (!created) {
                shortfall = true;
                continue;
            }
            Read& read = *created.value();
            read.owner = this;
            read.id = entry.id;
            read.variant = entry.variant;
            read.level = level_index;
            read.source = entry.source;
            read.offset = level.declared.offset;
            read.size = level.declared.bytes;
            if (Status sized = read.bytes.resize(level.declared.bytes); !sized) {
                shortfall = true;
                continue;
            }

            Expected<UniquePtr<Read>*, Error> stored =
                reads_.emplace_back(std::move(created.value()));
            if (!stored) {
                shortfall = true;
                continue;
            }
            level.in_flight = true;
            if (level.last_request_ns == kNeverRequested) {
                level.last_request_ns = now_ns;
            }
            // Counted BEFORE submission: the service thread decrements it, and a decrement that
            // could run before the increment would let `shutdown` free a cell mid-read.
            reads_in_flight_.fetch_add(1, std::memory_order_acq_rel);
            Expected<jobs::JobHandle, Error> submitted = async_->submit_blocking(
                &StreamingSystem::perform_read, static_cast<void*>(stored.value()->get()),
                "assets.stream.read");
            if (!submitted) {
                reads_in_flight_.fetch_sub(1, std::memory_order_acq_rel);
                level.in_flight = false;
                reads_.erase(reads_.size() - 1);
                ++stats_.requests_deferred;
                shortfall = true;
            }
        }
    }
    if (shortfall) {
        ++stats_.updates_with_shortfall;
    }
}

void StreamingSystem::evict_to_budget() noexcept {
    while (resident_bytes_total_ > budget_bytes_) {
        // The least recently REQUESTED level, over every asset, and never a base. The
        // specification's own eviction order.
        Entry* victim_entry = nullptr;
        u32 victim_level = 0;
        i64 oldest = 0;
        bool found = false;
        for (UniquePtr<Entry>& entry : entries_) {
            for (u32 index = 1; index < entry->levels.size(); ++index) {
                const Level& level = entry->levels[index];
                if (!level.resident) {
                    continue;
                }
                if (!found || level.last_request_ns < oldest) {
                    found = true;
                    oldest = level.last_request_ns;
                    victim_entry = entry.get();
                    victim_level = index;
                }
            }
        }
        if (!found) {
            // Everything resident is a base. Over budget and nothing may be dropped: the levels
            // that must never go are the ones the fallback guarantee is made of, so the budget is
            // exceeded and said so rather than a frame drawing nothing.
            return;
        }
        Level& level = victim_entry->levels[victim_level];
        level.bytes.clear();
        level.resident = false;
        resident_bytes_total_ -= level.declared.bytes;
        ++stats_.evictions;
        stats_.bytes_evicted += level.declared.bytes;
    }
}

void StreamingSystem::update(i64 now_ns) noexcept {
    if (!running_) {
        return;
    }
    collect_finished();
    evict_to_budget();
    admit(now_ns);
}

StreamingStats StreamingSystem::stats() const noexcept {
    StreamingStats snapshot = stats_;
    snapshot.resident_bytes = resident_bytes_total_;
    snapshot.budget_bytes = budget_bytes_;
    u64 levels = 0;
    for (const UniquePtr<Entry>& entry : entries_) {
        for (const Level& level : entry->levels) {
            if (level.resident) {
                ++levels;
            }
        }
    }
    snapshot.resident_levels = levels;
    return snapshot;
}

void StreamingSystem::reset_stats() noexcept {
    const u64 declared = stats_.declared;
    stats_ = StreamingStats{};
    stats_.declared = declared;
}

}  // namespace cy::assets
