// The generation table, and the registry of handle tag names. Task 1.3.2.

#include <cy/core/values/handle.h>

#include "counters.h"

#include <new>

namespace cy {
namespace {

/// Tag names, indexed by tag id. Tags are registered from function-local statics on first use of a
/// `Handle<Tag>`, so this is touched a handful of times per process and never on a lookup path.
std::mutex& tag_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::vector<Name>& tag_names() noexcept {
    // Index 0 is kInvalidHandleTag and has no name.
    static std::vector<Name> names(1);
    return names;
}

/// Round `value` up to a power of two, with a floor of 64. The table indexes a slot by shifting, so
/// the chunk size must be a power of two; the floor keeps a pathological argument from making one
/// chunk per handle.
u32 round_up_pow2(u32 value) noexcept {
    u32 result = 64;
    while (result < value && result < (1u << 20)) {
        result <<= 1;
    }
    return result;
}

u32 shift_of(u32 power_of_two) noexcept {
    u32 shift = 0;
    while ((1u << shift) < power_of_two) {
        ++shift;
    }
    return shift;
}

}  // namespace

namespace detail {

HandleTag register_handle_tag(const char* name) noexcept {
    const Name interned = Name::intern(name != nullptr ? name : "unnamed");
    const std::lock_guard<std::mutex> guard(tag_mutex());
    std::vector<Name>& names = tag_names();
    names.push_back(interned);
    return static_cast<HandleTag>(names.size() - 1);
}

}  // namespace detail

Name handle_tag_name(HandleTag tag) noexcept {
    const std::lock_guard<std::mutex> guard(tag_mutex());
    const std::vector<Name>& names = tag_names();
    return tag < names.size() ? names[tag] : Name{};
}

// --- GenerationTable -----------------------------------------------------------------------------

GenerationTable::GenerationTable(u32 slots_per_chunk) noexcept
    : slots_per_chunk_(round_up_pow2(slots_per_chunk)),
      chunk_shift_(shift_of(round_up_pow2(slots_per_chunk))),
      chunk_mask_(round_up_pow2(slots_per_chunk) - 1) {}

GenerationTable::~GenerationTable() {
    const u32 count = chunk_count_.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        delete[] chunks_[i].generations;
    }
}

std::atomic<u32>* GenerationTable::slot_ptr(u32 slot) const noexcept {
    const u32 chunk = slot >> chunk_shift_;
    // Acquire pairs with the release store in allocate(): seeing the count means seeing the chunk.
    if (chunk >= chunk_count_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return chunks_[chunk].generations + (slot & chunk_mask_);
}

Expected<u32, Error> GenerationTable::allocate() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);

    u32 slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = slot_count_.load(std::memory_order_relaxed);
        const u32 chunk = slot >> chunk_shift_;
        if (chunk >= kMaxChunks) {
            return fail(ErrorCode::OutOfRange, "handle table is full");
        }
        if (chunk >= chunk_count_.load(std::memory_order_relaxed)) {
            auto* generations = new (std::nothrow) std::atomic<u32>[slots_per_chunk_];
            if (generations == nullptr) {
                return fail(ErrorCode::OutOfMemory, "handle table chunk allocation failed");
            }
            for (u32 i = 0; i < slots_per_chunk_; ++i) {
                generations[i].store(0, std::memory_order_relaxed);
            }
            chunks_[chunk].generations = generations;
            // Release: a reader that sees this count sees a chunk whose counters are all written.
            chunk_count_.store(chunk + 1, std::memory_order_release);
            values::detail::bump(values::detail::counters().handle_chunks_committed);
        }
        slot_count_.store(slot + 1, std::memory_order_relaxed);
    }

    std::atomic<u32>* generation = slot_ptr(slot);
    // slot_ptr() cannot be null here: the chunk was just committed, or the slot came off the free
    // list and its chunk was committed when it was first allocated.
    //
    // PARITY IS THE LIVENESS BIT, and the counter only ever increases. An odd generation is a live
    // slot, an even one is free, and a slot that has never been touched holds 0 — even, free, and
    // also the null handle's generation, so a zeroed slot table hands out nothing.
    //
    // The counter must not be reset on free. A scheme that zeroed a freed slot and restarted at 1
    // would make every first reuse of every slot generation 1, and a handle kept from an earlier
    // cycle would then alias the new occupant — which is precisely the bug the counter exists to
    // prevent, reintroduced by the bookkeeping. It wraps after 2^31 reuses of one slot; that is the
    // documented limit of a 32-bit generation and is why the field is 32 bits rather than 16.
    const u32 previous = generation->load(std::memory_order_relaxed);
    generation->store(previous + 1, std::memory_order_release);

    live_count_.fetch_add(1, std::memory_order_relaxed);
    values::detail::bump(values::detail::counters().handle_slots_allocated);
    return slot;
}

Status GenerationTable::release(u32 slot) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);

    std::atomic<u32>* generation = slot_ptr(slot);
    if (generation == nullptr || slot >= slot_count_.load(std::memory_order_relaxed)) {
        return fail(ErrorCode::OutOfRange, "handle slot was never allocated");
    }
    const u32 current = generation->load(std::memory_order_relaxed);
    if ((current & 1u) == 0u) {
        return fail(ErrorCode::NotFound, "handle slot is already free");
    }

    // One increment: the slot becomes even, which is free, and every outstanding handle to it now
    // carries a generation that no longer matches.
    generation->store(current + 1, std::memory_order_release);
    free_slots_.push_back(slot);
    live_count_.fetch_sub(1, std::memory_order_relaxed);
    values::detail::bump(values::detail::counters().handle_slots_freed);
    return ok();
}

u32 GenerationTable::generation_of(u32 slot) const noexcept {
    const std::atomic<u32>* generation = slot_ptr(slot);
    if (generation == nullptr) {
        return 0;
    }
    const u32 current = generation->load(std::memory_order_acquire);
    return (current & 1u) != 0u ? current : 0;  // even is free, and free has no generation
}

bool GenerationTable::is_live(u32 slot, u32 generation) const noexcept {
    if ((generation & 1u) == 0u) {
        return false;  // 0 is the null handle; any other even value never named a live slot
    }
    const std::atomic<u32>* current = slot_ptr(slot);
    if (current == nullptr || current->load(std::memory_order_acquire) != generation) {
        stale_rejections_.fetch_add(1, std::memory_order_relaxed);
        values::detail::bump(values::detail::counters().stale_handle_rejections);
        return false;
    }
    return true;
}

u32 GenerationTable::capacity() const noexcept {
    return slot_count_.load(std::memory_order_relaxed);
}

u32 GenerationTable::live() const noexcept {
    return live_count_.load(std::memory_order_relaxed);
}

u64 GenerationTable::stale_rejections() const noexcept {
    return stale_rejections_.load(std::memory_order_relaxed);
}

}  // namespace cy
