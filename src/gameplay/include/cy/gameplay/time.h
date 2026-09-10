#pragma once
// Time domains and per-domain timers. M8.b task 3.3.
//
// `gameplay-framework` — "Time domains": the engine provides "**time domains** — real, gameplay,
// simulation, interface, cinematic, and project-defined — each with its own elapsed time, scale,
// and paused state. **There SHALL NOT be one global delta time that all systems consume.** Pausing
// SHALL be a per-domain policy: gameplay may pause while the interface animates, audio continues,
// and networking proceeds. A session MAY forbid pausing entirely. Systems SHALL declare the domain
// they advance with."
//
// And "The simulation clock": "Timers SHALL be provided per time domain, implemented so that many
// timers cost bounded work — a bucketed or wheel structure rather than one heap entry and one
// callback per timer per tick", and "Scheduled gameplay SHALL be expressed in **ticks**, not
// wall-clock time: 'at tick 8842' is reproducible; 'in 3.0 seconds' is not."
//
// ================================================================================================
// WHY THE TIMER STRUCTURE IS KEYED BY DUE TICK AND NOT BY A FIXED RING
// ================================================================================================
//
// The requirement's own measurement is "WHEN fifty thousand timers are pending THEN advancing a
// tick SHALL cost work proportional to the timers **actually due**." A fixed ring of N buckets does
// not satisfy that: fifty thousand timers spread over a long horizon put fifty thousand divided by
// N of them in the bucket a tick lands on, and advancing examines all of them to find that none is
// due. So the bucket key here is the due tick itself, held in a hash table of intrusive lists: an
// advance is one lookup and a walk of exactly the entries due.
//
// `TimerWheel::advanced_examined()` reports what the last advance touched, so the property is
// measured by a test rather than argued in a comment — `tests/test_time.cpp` schedules fifty
// thousand and asserts the count.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>

namespace cy::gameplay {

/// The domains the engine names. A project adds its own by declaring a `Name`; there is no
/// enumeration to extend, which is what "and project-defined" requires.
enum class TimeDomainKind : u8 {
    /// Never scaled and never paused. What a profiler and a frame limiter read.
    Real = 0,
    /// The one a pause menu stops.
    Gameplay,
    /// The fixed-step domain. Advanced in ticks by the simulation, not in seconds by a frame.
    Simulation,
    /// Animates while gameplay is paused.
    Interface,
    Cinematic,
    Count,
};

const char* time_domain_kind_name(TimeDomainKind kind) noexcept;

using DomainId = u32;
inline constexpr DomainId kInvalidDomain = 0xFFFFFFFFU;

/// One domain's clock.
struct TimeDomain {
    Name name;
    f64 elapsed = 0.0;
    f32 delta = 0.0F;
    f32 scale = 1.0F;
    bool paused = false;
    /// Real time ignores scale and pause. Declared rather than special-cased on the enumerator, so
    /// that a project-defined "audio" domain can be unpausable too.
    bool unscalable = false;
};

/// Every domain of one session. **There is no global delta time here** and no way to ask for one:
/// `delta()` takes a domain because a system declares the domain it advances with.
class TimeDomains {
public:
    static constexpr u32 kMaxDomains = 16;

    explicit TimeDomains(Allocator& allocator) noexcept;

    TimeDomains(const TimeDomains&) = delete;
    TimeDomains& operator=(const TimeDomains&) = delete;

    /// The engine's five, declared at construction. `builtin()` is how a system names one.
    [[nodiscard]] DomainId builtin(TimeDomainKind kind) const noexcept;
    /// A project's own domain.
    [[nodiscard]] Expected<DomainId, Error> declare(Name name, bool unscalable = false) noexcept;
    [[nodiscard]] DomainId find(Name name) const noexcept;

    [[nodiscard]] Status set_scale(DomainId domain, f32 scale) noexcept;
    [[nodiscard]] Status set_paused(DomainId domain, bool paused) noexcept;

    /// A session may forbid pausing entirely. Refuses the pause rather than ignoring it, so that a
    /// host that asked can tell it did not happen.
    void set_pause_allowed(bool allowed) noexcept { pause_allowed_ = allowed; }
    [[nodiscard]] bool pause_allowed() const noexcept { return pause_allowed_; }

    /// Advance every domain by one real frame. Each applies its own scale and pause.
    void advance(f32 real_delta) noexcept;

    [[nodiscard]] f32 delta(DomainId domain) const noexcept;
    [[nodiscard]] f64 elapsed(DomainId domain) const noexcept;
    [[nodiscard]] f32 scale(DomainId domain) const noexcept;
    [[nodiscard]] bool paused(DomainId domain) const noexcept;
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(domains_.size()); }
    [[nodiscard]] const TimeDomain& at(DomainId domain) const noexcept { return domains_[domain]; }

private:
    Array<TimeDomain> domains_;
    DomainId builtins_[static_cast<usize>(TimeDomainKind::Count)] = {};
    bool pause_allowed_ = true;
};

using TimerId = u32;
inline constexpr TimerId kInvalidTimer = 0xFFFFFFFFU;

/// A timer that fired.
struct TimerExpiry {
    TimerId id = kInvalidTimer;
    u64 due_tick = 0;
    /// The caller's own handle for what should happen. **Not a callback**: a callback per timer per
    /// tick is exactly the shape the requirement rules out, and a `u64` lets the caller index its
    /// own table.
    u64 user = 0;
};

/// Timers due at exact ticks, bucketed by due tick.
///
/// SCHEDULING IS IN TICKS. There is no seconds-taking overload, because "in 3.0 seconds" is not
/// reproducible and a convenience that converts one would make it look as though it were.
class TimerWheel {
public:
    explicit TimerWheel(Allocator& allocator) noexcept;

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;
    TimerWheel(TimerWheel&&) noexcept = default;
    TimerWheel& operator=(TimerWheel&&) noexcept = default;

    [[nodiscard]] Expected<TimerId, Error> schedule(u64 due_tick, u64 user) noexcept;
    /// Cancel. A cancelled timer stays in its bucket until the tick it was due, and is skipped —
    /// removing it would cost a list walk to buy nothing, since the bucket is visited exactly once.
    bool cancel(TimerId id) noexcept;

    /// Fire everything due at or before `tick`, oldest tick first and, within a tick, in the order
    /// they were scheduled. The order is declared because a replay has to reproduce it.
    [[nodiscard]] u32 advance_to(u64 tick, TimerExpiry* out, u32 capacity) noexcept;

    /// How many entries the last `advance_to` looked at. The requirement's measurement.
    [[nodiscard]] u32 last_examined() const noexcept { return examined_; }
    [[nodiscard]] u32 pending() const noexcept { return pending_; }
    [[nodiscard]] u64 now() const noexcept { return now_; }

private:
    struct Entry {
        u64 due_tick = 0;
        u64 user = 0;
        u32 next = kInvalidTimer;
        bool cancelled = false;
    };

    Array<Entry> entries_;
    /// Due tick -> the first entry due at it. The entries themselves are an intrusive list, so a
    /// bucket costs one `u32` however many timers land in it.
    HashMap<u64, u32> heads_;
    /// Due tick -> the last entry, so scheduling appends in order without a walk.
    HashMap<u64, u32> tails_;
    /// The occupied due ticks as a min-heap, so an advance finds the next one without scanning the
    /// ticks between. Without it, advancing from tick 0 to tick 1 000 000 would probe a million
    /// empty buckets — which is bounded work per tick and absurd work per advance.
    Array<u64> due_;
    u64 now_ = 0;
    u32 pending_ = 0;
    u32 examined_ = 0;
};

/// One timer wheel per domain. `gameplay-framework`: "Timers SHALL be provided per time domain."
class TimerService {
public:
    TimerService(Allocator& allocator, const TimeDomains& domains) noexcept;

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    [[nodiscard]] Status add_domain(DomainId domain) noexcept;
    [[nodiscard]] TimerWheel* wheel(DomainId domain) noexcept;
    [[nodiscard]] const TimerWheel* wheel(DomainId domain) const noexcept;

private:
    struct Row {
        DomainId domain = kInvalidDomain;
        TimerWheel wheel;
    };

    Allocator* allocator_;
    const TimeDomains* domains_;
    Array<Row> wheels_;
};

}  // namespace cy::gameplay
