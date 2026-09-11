// Snapshot kinds, checkpoints, and the bounded rollback window. M9 task 1.3.

#include <cy/replay/snapshot.h>

#include <cy/core/memory/allocator.h>
#include <cy/replay/log.h>

#include <algorithm>
#include <cstring>

namespace cy::replay {
namespace {

using determinism::Participates;
using determinism::participates_in;

}  // namespace

const char* snapshot_kind_name(SnapshotKind kind) noexcept {
    switch (kind) {
        case SnapshotKind::Rollback:
            return "Rollback";
        case SnapshotKind::ReplayCheckpoint:
            return "ReplayCheckpoint";
        case SnapshotKind::SaveCheckpoint:
            return "SaveCheckpoint";
        case SnapshotKind::DebugCapture:
            return "DebugCapture";
    }
    return "Rollback";
}

const char* snapshot_encoding_name(SnapshotEncoding encoding) noexcept {
    switch (encoding) {
        case SnapshotEncoding::CurrentLayoutInMemory:
            return "CurrentLayoutInMemory";
        case SnapshotEncoding::CurrentLayoutCompressed:
            return "CurrentLayoutCompressed";
        case SnapshotEncoding::TaggedVersioned:
            return "TaggedVersioned";
        case SnapshotEncoding::Rich:
            return "Rich";
    }
    return "CurrentLayoutInMemory";
}

const char* window_refusal_name(WindowRefusal refusal) noexcept {
    switch (refusal) {
        case WindowRefusal::None:
            return "None";
        case WindowRefusal::RequiresResynchronisation:
            return "RequiresResynchronisation";
        case WindowRefusal::InTheFuture:
            return "InTheFuture";
        case WindowRefusal::Empty:
            return "Empty";
    }
    return "None";
}

bool StateCapture::holds_in_memory_snapshot() const noexcept {
    return kind_ == SnapshotKind::Rollback || kind_ == SnapshotKind::ReplayCheckpoint;
}

bool StateCapture::holds_tagged_stream() const noexcept {
    return !holds_in_memory_snapshot();
}

u64 StateCapture::bytes() const noexcept {
    return entities_.bytes() + static_cast<u64>(stream_.size()) +
           static_cast<u64>(provider_bytes_.size());
}

Status StateCapture::capture(ecs::World& world, const determinism::StateProviderRegistry& registry,
                             determinism::SimulationPoint at) noexcept {
    if (!registry.finalized()) {
        return fail(ErrorCode::Unavailable,
                    "replay: a capture reads providers in the registry's finalised order; an "
                    "unfinalised registry has an order that depends on when plugins loaded");
    }
    clear();
    at_ = at;

    // THE ENCODINGS DO NOT OVERLAP, AND THAT IS THE REQUIREMENT. A rollback capture holds an
    // `ecs::Snapshot` and no stream; a save capture holds a stream and no snapshot. "The save
    // encoding SHALL NOT be used for rollback" is therefore a property of the object rather than a
    // rule someone has to follow, and `holds_in_memory_snapshot()`/`holds_tagged_stream()` are what
    // tests/test_snapshot.cpp checks it with.
    if (holds_in_memory_snapshot()) {
        if (Status captured = entities_.capture(world); !captured) {
            return captured;
        }
    } else {
        if (Status written = ecs::serialize(world, stream_); !written) {
            return written;
        }
    }

    const Participates wanted = participation_for(kind_);
    for (u32 index = 0; index < registry.size(); ++index) {
        determinism::StateProvider& provider = registry.at(index);
        if (!participates_in(provider.participation(), wanted)) {
            ++providers_declined_;
            continue;
        }
        ProviderSlice slice;
        slice.name = provider.name();
        slice.offset = static_cast<u32>(provider_bytes_.size());
        if (Status captured = provider.capture(provider_bytes_); !captured) {
            return captured;
        }
        slice.size = static_cast<u32>(provider_bytes_.size()) - slice.offset;
        if (Status added = providers_.push_back(slice); !added) {
            return added;
        }
        ++providers_captured_;
    }
    provider_bytes_plain_ = static_cast<u32>(provider_bytes_.size());

    if (kind_ == SnapshotKind::ReplayCheckpoint && provider_bytes_plain_ != 0) {
        // The one kind whose provider half is compressed. Deterministic, so two identical sessions
        // produce identical checkpoint bytes — see log.h's note on why that matters.
        Array<u8> packed(provider_bytes_.allocator());
        if (Status compressed = compress(provider_bytes_.span(), packed); !compressed) {
            return compressed;
        }
        provider_bytes_.clear();
        if (Status appended = provider_bytes_.append(packed.span()); !appended) {
            return appended;
        }
        compressed_ = true;
    }

    captured_ = true;
    return ok();
}

Status StateCapture::restore(ecs::World& world,
                             const determinism::StateProviderRegistry& registry) const noexcept {
    if (!captured_) {
        return fail(ErrorCode::Unavailable, "replay: nothing has been captured");
    }
    if (holds_tagged_stream()) {
        // Refused, and the reason is not squeamishness. `ecs::deserialize` *appends* fresh
        // entities: it restores values under new identifiers. `simulation-and-determinism` folds
        // entity identity into the state hash, so restoring this way would report a divergence on
        // every entity restored. Loading a save into a fresh world is `save-and-persistence`'s
        // path, and it is the case this encoding is correct for.
        return fail(ErrorCode::Unsupported,
                    "replay: a SaveCheckpoint or DebugCapture is not restored through the rollback "
                    "path — the tagged stream mints fresh entity identifiers, and identity is part "
                    "of the state hash");
    }
    if (Status restored = entities_.restore(world); !restored) {
        return restored;
    }

    Array<u8> plain(provider_bytes_.allocator());
    Span<const u8> source = provider_bytes_.span();
    if (compressed_) {
        if (Status expanded = decompress(provider_bytes_.span(), provider_bytes_plain_, plain);
            !expanded) {
            return expanded;
        }
        source = plain.span();
    }

    for (const ProviderSlice& slice : providers_) {
        determinism::StateProvider* provider = registry.find(slice.name);
        if (provider == nullptr) {
            return fail(ErrorCode::NotFound,
                        "replay: a capture names a state provider this registry does not have; a "
                        "restore that skipped it would resume with that subsystem at whatever it "
                        "happened to hold");
        }
        if (Status restored =
                provider->restore(Span<const u8>(source.data() + slice.offset, slice.size));
            !restored) {
            return restored;
        }
    }
    return ok();
}

void StateCapture::clear() noexcept {
    stream_.clear();
    provider_bytes_.clear();
    providers_.clear();
    providers_captured_ = 0;
    providers_declined_ = 0;
    provider_bytes_plain_ = 0;
    compressed_ = false;
    captured_ = false;
}

// --- Checkpoint policy ---------------------------------------------------------------------------

u32 CheckpointPolicy::chosen_interval(const CheckpointState& state) const noexcept {
    u32 interval = std::max(max_tick_interval, min_tick_interval);

    // Expected seeking behaviour: a viewer who seeks a minute at a time gains nothing from a
    // checkpoint every second, and one who scrubs frame by frame gains everything.
    if (expected_seek_ticks != 0 && expected_seek_ticks < interval) {
        interval = static_cast<u32>(expected_seek_ticks);
    }

    // State size against the storage budget: a big world inside a small budget gets a longer
    // interval rather than a truncated session. Zero budget means unbounded, which is the honest
    // encoding of "the caller has not said".
    if (storage_budget != 0 && state.last_capture_bytes != 0) {
        const u64 affordable = storage_budget / state.last_capture_bytes;
        // Two ways to be out of room and one answer: no space for even one more capture at this
        // size, or the budget already spent. Either way the interval stretches to the longest the
        // policy allows rather than the session being truncated — a replay with fewer checkpoints
        // is slower to seek, and a replay that stopped recording is gone.
        if (affordable == 0 || state.storage_used >= storage_budget) {
            interval = max_tick_interval;
        }
    }

    return std::max(interval, min_tick_interval);
}

bool CheckpointPolicy::due(const CheckpointState& state) const noexcept {
    if (state.ticks_since_last < min_tick_interval) {
        // The floor wins over everything. A command storm must not be able to ask for a checkpoint
        // every tick.
        return false;
    }
    if (command_volume_trigger != 0 && state.commands_since_last >= command_volume_trigger) {
        // Seeking cost is the *commands* between checkpoints, not the ticks: a tick with two
        // hundred commands in it is two hundred times the re-simulation of a tick with one.
        return true;
    }
    return state.ticks_since_last >= chosen_interval(state);
}

// --- The bounded rollback window
// ------------------------------------------------------------------

SnapshotRing::~SnapshotRing() {
    clear();
}

Status SnapshotRing::capture(ecs::World& world, const determinism::StateProviderRegistry& registry,
                             determinism::SimulationPoint at) noexcept {
    void* storage = allocator_->allocate(sizeof(StateCapture), alignof(StateCapture));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "replay: no room for a rollback capture");
    }
    auto* capture = construct_at<StateCapture>(storage, *allocator_, SnapshotKind::Rollback);
    if (Status captured = capture->capture(world, registry, at); !captured) {
        capture->~StateCapture();
        allocator_->deallocate(storage, sizeof(StateCapture), alignof(StateCapture));
        return captured;
    }
    if (Status added = captures_.push_back(capture); !added) {
        capture->~StateCapture();
        allocator_->deallocate(storage, sizeof(StateCapture), alignof(StateCapture));
        return added;
    }
    evict_to_budget();
    return ok();
}

void SnapshotRing::evict_to_budget() noexcept {
    // Bounded by bytes rather than by count: a world that doubles in size halves the window it can
    // afford, and a ring of sixty captures would simply use twice the memory and tell nobody.
    // The newest is never evicted — a window of one is short, and no window at all is a different
    // failure.
    //
    // SHIFT AND `pop_back()`, NEVER `resize(size() - 1)`. The obvious spelling compiles at `-O0`
    // and FAILS the Profile and Shipping builds: GCC 13 at `-O2` loses the loop condition's
    // guarantee that the size is at least two, concludes the subtraction may wrap, and reports a
    // `memset` of 18 446 744 073 709 551 608 bytes inside `Array::resize`'s growth path — which it
    // reaches only when the new count is larger. `pop_back()` has no arithmetic to widen. Caught by
    // building all four profiles rather than only the one that is quick.
    while (captures_.size() > 1 && bytes() > budget_) {
        const usize held = captures_.size();
        StateCapture* oldest = captures_[0];
        for (usize index = 1; index < held; ++index) {
            captures_[index - 1] = captures_[index];
        }
        captures_.pop_back();
        oldest->~StateCapture();
        allocator_->deallocate(oldest, sizeof(StateCapture), alignof(StateCapture));
        ++evictions_;
    }
}

const StateCapture* SnapshotRing::find(determinism::SimulationPoint at,
                                       WindowRefusal& refusal) const noexcept {
    refusal = WindowRefusal::None;
    if (captures_.empty()) {
        refusal = WindowRefusal::Empty;
        return nullptr;
    }
    if (at.tick < captures_[0]->point().tick) {
        // "A request older than the window SHALL be reported as requiring full resynchronisation
        // rather than triggering an unbounded replay."
        refusal = WindowRefusal::RequiresResynchronisation;
        return nullptr;
    }
    const StateCapture* best = nullptr;
    for (StateCapture* capture : captures_) {
        if (capture->point().tick <= at.tick) {
            best = capture;
        }
    }
    if (best == nullptr) {
        refusal = WindowRefusal::InTheFuture;
    }
    return best;
}

u64 SnapshotRing::bytes() const noexcept {
    u64 total = 0;
    for (const StateCapture* capture : captures_) {
        total += capture->bytes();
    }
    return total;
}

u64 SnapshotRing::window_ticks() const noexcept {
    if (captures_.size() < 2) {
        return 0;
    }
    return captures_[captures_.size() - 1]->point().tick - captures_[0]->point().tick;
}

void SnapshotRing::clear() noexcept {
    for (StateCapture* capture : captures_) {
        capture->~StateCapture();
        allocator_->deallocate(capture, sizeof(StateCapture), alignof(StateCapture));
    }
    captures_.clear();
}

}  // namespace cy::replay
