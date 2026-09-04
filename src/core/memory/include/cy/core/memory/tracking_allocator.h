#pragma once
// The debug wrapper: tag, size, call site, red zones, poison and double-free detection.
// Tasks 2.1 and 2.11.
//
// `core-memory-and-containers` — "Allocator interface": `TrackingAllocator` is the debug wrapper
// recording tag, size and call site. "Memory diagnostics" adds what it is for: leak reporting at
// shutdown with the allocating call site, optional guard pages or red zones, poisoning of freed
// memory, and double-free validation — with call-stack capture a DECLARED MODE (off, sampled or
// full) rather than an always-on cost.
//
// It wraps any allocator, so the thing being measured is unchanged by being measured: the
// allocations still come from the arena, pool or heap they came from, and this only sees them pass.
//
// THE HEADER IS IN FRONT OF THE BLOCK. Every tracked allocation is one upstream allocation of
// header + padding + payload + red zone, so there is no side table to keep consistent and a
// pointer's record is found by subtraction rather than by lookup. The cost is real — a header per
// allocation — and is exactly why this is a wrapper a build opts into rather than the allocator.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy {

/// How much of an allocation's origin is recorded.
///
/// `Sampled` records one in every `sample_interval` allocations, which is what makes a long
/// development session affordable: the memory a report needs is the shape of the allocations, and
/// the shape survives sampling.
enum class CaptureMode : u8 {
    Off = 0,
    Sampled = 1,
    Full = 2,
};

[[nodiscard]] const char* capture_mode_name(CaptureMode mode) noexcept;

/// Where an allocation came from, as the source said it. Pushed by `CY_ALLOCATION_SITE`, so a
/// container that allocates on a caller's behalf records the caller rather than itself.
struct AllocationSite {
    const char* file = "";
    const char* function = "";
    u32 line = 0;
};

/// The site in force on this thread. Never null.
[[nodiscard]] const AllocationSite& current_allocation_site() noexcept;

/// Push a site for the duration of a scope.
class AllocationSiteScope {
public:
    explicit AllocationSiteScope(const AllocationSite& site) noexcept;
    ~AllocationSiteScope();

    AllocationSiteScope(const AllocationSiteScope&) = delete;
    AllocationSiteScope& operator=(const AllocationSiteScope&) = delete;
    AllocationSiteScope(AllocationSiteScope&&) = delete;
    AllocationSiteScope& operator=(AllocationSiteScope&&) = delete;

private:
    const AllocationSite* previous_;
};

/// Attribute every allocation made in this scope to this line.
///
///   CY_ALLOCATION_SITE();
///   auto buffer = mesh_vertices.reserve(count);   // recorded at this file and line
#define CY_ALLOCATION_SITE()                                                                     \
    static constexpr ::cy::AllocationSite cy_allocation_site_{__FILE__, __func__,                \
                                                              static_cast<::cy::u32>(__LINE__)}; \
    const ::cy::AllocationSiteScope cy_allocation_site_scope_ {                                  \
        cy_allocation_site_                                                                      \
    }

/// One live allocation, as the leak report presents it.
struct TrackedAllocation {
    const void* pointer = nullptr;
    u64 bytes = 0;
    AllocationTag tag = "";
    AllocationSite site;
    u64 sequence = 0;  // allocation order, so a report can say which came first
    bool process_lifetime = false;
};

/// What a leak report found.
struct LeakReport {
    u64 live_allocations = 0;
    u64 live_bytes = 0;
    u64 process_lifetime_allocations = 0;
    u64 process_lifetime_bytes = 0;
    /// Live and not declared as process-lifetime: the ones that are actually a defect.
    u64 leaked_allocations = 0;
    u64 leaked_bytes = 0;
};

/// A sink a report is written to, so this module does not have to know whether the destination is
/// the log, a file or a test's own buffer. Called once per leaked allocation.
using LeakSink = void (*)(const TrackedAllocation& allocation, void* user);

class TrackingAllocator final : public Allocator {
public:
    /// A red zone this wide is written after every payload and checked on free. 32 bytes catches
    /// the overruns a header-based tracker can catch at all; the ones it cannot are what
    /// AddressSanitizer is for, and the two are complementary rather than alternatives.
    static constexpr usize kRedZoneBytes = 32;

    /// How many recently freed pointers are remembered so that freeing one again is caught WITHOUT
    /// reading the block. See the note on double-free detection below.
    static constexpr u32 kRecentFrees = 64;

    TrackingAllocator(Allocator& upstream, MemoryDomain domain, AllocationTag tag) noexcept;
    ~TrackingAllocator() override;

    void set_capture_mode(CaptureMode mode, u32 sample_interval = 64) noexcept;
    [[nodiscard]] CaptureMode capture_mode() const noexcept { return capture_; }

    /// Whether a red zone is written and checked. On by default: the cost is one memset of 32 bytes
    /// and one comparison, and the class exists to find this class of bug.
    void set_red_zones_enabled(bool enabled) noexcept { red_zones_ = enabled; }

    /// Whether freed payloads are filled with a poison pattern before being returned upstream.
    void set_poison_on_free(bool enabled) noexcept { poison_ = enabled; }

    [[nodiscard]] u64 live_allocations() const noexcept { return live_allocations_; }
    [[nodiscard]] u64 live_bytes() const noexcept { return live_bytes_; }
    [[nodiscard]] u64 peak_bytes() const noexcept { return peak_bytes_; }
    [[nodiscard]] u64 total_allocations() const noexcept { return sequence_; }
    /// Frees of a pointer this allocator did not hand out, or handed out and already took back.
    [[nodiscard]] u64 double_frees() const noexcept { return double_frees_; }
    /// Red zones found overwritten at free.
    [[nodiscard]] u64 overruns() const noexcept { return overruns_; }

    /// Walk the live allocations, newest first. `sink` may be null, in which case only the totals
    /// are computed. Allocations declared through `lifetime.h` are counted separately and are NOT
    /// passed to the sink: the specification requires the report exclude them.
    LeakReport report_leaks(LeakSink sink, void* user) const noexcept;

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override;
    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override;
    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override;

private:
    /// Sits immediately before the payload. `padding` is the distance back to the block upstream
    /// actually returned, which is what `deallocate` needs and what alignment made variable.
    ///
    /// Never constructed: a header is a view placed over bytes the upstream allocator returned, and
    /// every field is written by `do_allocate` immediately afterwards. Default member initialisers
    /// here would be a claim that a constructor runs, and none does.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct Header {
        u64 magic;
        Header* previous;
        Header* next;
        usize payload_bytes;
        usize block_bytes;
        usize padding;
        usize alignment;
        AllocationTag tag;
        AllocationSite site;
        u64 sequence;
    };

    [[nodiscard]] bool should_capture() const noexcept;
    void unlink(Header& header) noexcept;

    /// DOUBLE-FREE DETECTION, AND ITS ONE LIMIT. A freed block goes back to the upstream allocator,
    /// so its header is memory this allocator no longer owns — and under AddressSanitizer, reading
    /// it is itself a use-after-free report. The recent-free ring is therefore checked FIRST, and
    /// it catches the case that actually happens: the same pointer freed twice.
    ///
    /// A free of a pointer freed longer than `kRecentFrees` frees ago, or one this allocator never
    /// handed out, still falls through to the header read. That read is unsafe, and under a
    /// sanitizer it produces a report naming both frees — which is a better diagnostic than this
    /// counter, so the fall-through is deliberate rather than an omission.
    [[nodiscard]] bool was_recently_freed(const void* pointer) const noexcept;
    void remember_free(const void* pointer) noexcept;

    Allocator* upstream_;
    Header* live_ = nullptr;  // newest first
    u64 live_allocations_ = 0;
    u64 live_bytes_ = 0;
    u64 peak_bytes_ = 0;
    u64 sequence_ = 0;
    u64 double_frees_ = 0;
    u64 overruns_ = 0;
    const void* recent_frees_[kRecentFrees] = {};
    u32 recent_free_next_ = 0;
    CaptureMode capture_ = CaptureMode::Full;
    u32 sample_interval_ = 64;
    bool red_zones_ = true;
    bool poison_ = true;
};

}  // namespace cy
