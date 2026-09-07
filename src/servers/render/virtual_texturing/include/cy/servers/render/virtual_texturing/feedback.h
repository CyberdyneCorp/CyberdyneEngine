#pragma once
// GPU feedback: the request stream that must never make a frame wait. Tasks 5.3 and 5.4.
//
// `virtual-texturing` — "GPU feedback": shaders record the virtual pages they would have sampled,
// and the results "are deduplicated, compacted, and prioritised on the GPU before any CPU
// involvement". **"A per-pixel request stream SHALL NOT reach the CPU."** Density is a quality
// lever. And M6's exit criterion, which this file exists to make true: **feedback never blocks a
// frame.**
//
// --- WHAT "NEVER BLOCKS" MEANS HERE, PRECISELY
// ----------------------------------------------------
//
// `record()` is wait-free. It performs two atomic increments, a bounds test and one store, in a
// bounded number of instructions, with no lock, no allocation, no retry loop and no call that could
// take one. There is no path through it that waits for another thread, and no input that makes it
// take longer. When the buffer is full it drops the sample and counts the drop — because the
// alternative to dropping a page request is stalling the frame that produced it, and a dropped
// request costs a blurrier surface for one frame while a stall costs the frame.
//
// `swap()` is the one call that waits, and it waits for at most the writers that were already
// inside `record()` when the flip happened — one atomic store each. It is called by the streaming
// side once a frame, off the recording path. Its spin count is reported (`drain_spins()`) so that
// "bounded" is a number a test reads rather than an adjective.
//
// --- THE HANDOFF, AND WHY IT IS TWO BANKS
// ------------------------------------------------------------
//
// The frame writes bank A while the resolver reads bank B. `swap()` flips which is which and drains
// the retired bank's in-flight writers before returning; `resolve()` then reads a bank nobody can
// still be writing to. A single buffer with a lock would satisfy correctness and violate the
// requirement; a single buffer without one would be a data race.
//
// The writer/flipper handshake is the Dekker pattern and is written with sequentially consistent
// ordering on exactly the two pairs that need it: the writer increments its bank's writer count and
// then re-reads which bank is active, and the flipper stores the new active bank and then reads the
// retired one's writer count. A writer that arrives after the flip sees it and backs out; a writer
// that arrived before it is seen by the flipper and waited for. Relaxing either of those four
// operations reintroduces the race, so neither is a candidate for tuning.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/render/virtual_texturing/address.h>

#include <atomic>

namespace cy::render::vt {

/// One compacted request: the page, and how many samples asked for it. The count is the whole
/// justification for compaction — "a million pixels sample one page" becomes one entry with a
/// million in it, and the count is a scoring input rather than a million scheduler entries.
struct FeedbackRequest {
    u64 address = 0;  // encoded VirtualAddress
    u32 samples = 0;
};

/// Feedback density: how many pixels share one sample. `virtual-texturing`: density "SHALL be
/// adjustable — one sample per pixel block rather than per pixel — and SHALL be a quality lever
/// driven by camera motion, texture pressure, resolution, and budget".
inline constexpr u32 kMinFeedbackDensity = 1;
inline constexpr u32 kMaxFeedbackDensity = 256;

class FeedbackBuffer {
public:
    explicit FeedbackBuffer(Allocator& allocator = current_allocator()) noexcept
        : banks_{Array<u64>(allocator), Array<u64>(allocator)}, seen_(allocator) {}

    FeedbackBuffer(const FeedbackBuffer&) = delete;
    FeedbackBuffer& operator=(const FeedbackBuffer&) = delete;
    FeedbackBuffer(FeedbackBuffer&&) = delete;
    FeedbackBuffer& operator=(FeedbackBuffer&&) = delete;
    ~FeedbackBuffer() = default;

    /// Size both banks. Called once at configuration; `record()` never allocates, which is half of
    /// why it cannot block.
    Status configure(u32 capacity) noexcept;
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// Record one page the shader would have sampled. WAIT-FREE; see the header note. Returns
    /// false when the sample was dropped, which is information and not an error.
    bool record(u64 encoded_address) noexcept;

    /// Whether this pixel is one of the ones that reports, at the current density. The density
    /// lever expressed as a predicate, so the caller's loop stays branch-simple.
    [[nodiscard]] bool samples_pixel(u32 pixel_index) const noexcept {
        return (pixel_index % density_.load(std::memory_order_relaxed)) == 0;
    }

    void set_density(u32 pixels_per_sample) noexcept;
    [[nodiscard]] u32 density() const noexcept { return density_.load(std::memory_order_relaxed); }

    /// Retire the bank being written and make the other one current. Drains the writers that were
    /// already inside `record()`. Off the frame's recording path; see the header note.
    void swap() noexcept;

    /// Deduplicate and count the retired bank. The CPU mirror of the GPU-side compaction: what
    /// reaches the scheduler is one entry per page, never one per sample.
    Status resolve(Array<FeedbackRequest>& out) noexcept;

    [[nodiscard]] u64 recorded() const noexcept {
        return recorded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 swaps() const noexcept { return swaps_; }
    /// How many times a `swap()` had to spin waiting for an in-flight writer. Reported so that
    /// "bounded" is a measurement.
    [[nodiscard]] u64 drain_spins() const noexcept {
        return drain_spins_.load(std::memory_order_relaxed);
    }

    void reset() noexcept;

private:
    // The banks take the CONSTRUCTOR'S allocator, which a default member initialiser cannot name —
    // it would have to spell `current_allocator()` and would then ignore what the caller passed.
    // NOLINTNEXTLINE(modernize-use-default-member-init)
    Array<u64> banks_[2];
    std::atomic<u32> heads_[2] = {};
    std::atomic<u32> writers_[2] = {};
    std::atomic<u32> active_{0};
    std::atomic<u32> density_{kMinFeedbackDensity};
    std::atomic<u64> recorded_{0};
    std::atomic<u64> dropped_{0};
    std::atomic<u64> drain_spins_{0};
    /// The deduplication table, owned by the resolver's thread alone. Kept between frames so that
    /// resolving allocates nothing in the steady state.
    HashMap<u64, u32> seen_;
    u32 capacity_ = 0;
    u32 retired_ = 1;
    u64 swaps_ = 0;
};

}  // namespace cy::render::vt
