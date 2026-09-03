#pragma once
// The job system's vocabulary: priorities, handles, deadlines, worker indices.
//
// Section 3.2, `core-jobs-and-concurrency`. Everything here is a value type with no storage of its
// own, so a header that only needs to *name* a job does not acquire the scheduler.
//
// The aliases below are the `base` types re-exported into `cy::jobs`, exactly as
// src/core/diagnostics/include/cy/core/diagnostics/prelude.h does it: the same types, not a
// parallel set, so that a jobs source reads the same whether it names a type or a function.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::jobs {

using u8 = ::cy::u8;
using u16 = ::cy::u16;
using u32 = ::cy::u32;
using u64 = ::cy::u64;
using i32 = ::cy::i32;
using i64 = ::cy::i64;
using f32 = ::cy::f32;
using f64 = ::cy::f64;
using usize = ::cy::usize;

using ErrorCode = ::cy::ErrorCode;

// `Error` is deliberately not re-exported — the error model belongs to base, and a signature here
// spells it `cy::Error`, which is the honest spelling.
template <class T, class E = ::cy::Error>
using Expected = ::cy::Expected<T, E>;

using ::cy::fail;
using Status = ::cy::Status;
using ::cy::ok;

/// Which worker a task ran on. It is a diagnostic and a scratch-arena selector, never an ordering
/// key: work stealing makes a worker's identity a function of timing, and the determinism
/// requirement forbids a commit order that depends on it.
using WorkerIndex = u32;

/// The value a worker index takes on a thread the job system does not own — the main thread, a
/// dedicated thread, a test's own thread.
inline constexpr WorkerIndex kNotAWorker = 0xFFFF'FFFFu;

/// A task's priority class. `core-jobs-and-concurrency` fixes these five names and their meaning:
/// they influence **when** work runs and never **what** the simulation computes.
enum class Priority : u8 {
    Critical = 0,
    High = 1,
    Normal = 2,
    Background = 3,
    Idle = 4,
};

inline constexpr u32 kPriorityCount = 5;

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* priority_name(Priority priority) noexcept;

/// A hint about when a result is wanted. The scheduler MAY use it to order work; it is never a
/// correctness contract, and deterministic mode ignores it entirely.
struct Deadline {
    /// The frame by which the result is wanted. Zero means "no frame is named".
    u64 frame_index = 0;
    /// Nanoseconds on the monotonic clock by which the result is wanted. Zero means unset.
    i64 time_ns = 0;

    [[nodiscard]] constexpr bool is_set() const noexcept {
        return frame_index != 0 || time_ns != 0;
    }
};

/// A reference to a submitted job.
///
/// Index and generation, packed into 64 bits, for the same reason `cy::Handle` in core-values is:
/// a task record is recycled, and a handle kept across the recycle must be recognisable as stale
/// rather than resolve to whatever now occupies the slot. Generation 0 is the null handle, and a
/// stale handle reads as *complete* — the job it named finished before the slot was reused, which
/// is the only condition under which a slot is ever reused.
class JobHandle {
public:
    constexpr JobHandle() noexcept = default;

    static constexpr JobHandle from_slot(u32 index, u32 generation) noexcept {
        return JobHandle{(static_cast<u64>(generation) << 32) | index};
    }
    static constexpr JobHandle from_bits(u64 bits) noexcept { return JobHandle{bits}; }

    [[nodiscard]] constexpr u32 index() const noexcept { return static_cast<u32>(bits_); }
    [[nodiscard]] constexpr u32 generation() const noexcept {
        return static_cast<u32>(bits_ >> 32);
    }
    [[nodiscard]] constexpr u64 bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool is_null() const noexcept { return generation() == 0; }

    friend constexpr bool operator==(JobHandle lhs, JobHandle rhs) noexcept {
        return lhs.bits_ == rhs.bits_;
    }
    friend constexpr bool operator!=(JobHandle lhs, JobHandle rhs) noexcept {
        return lhs.bits_ != rhs.bits_;
    }

private:
    explicit constexpr JobHandle(u64 bits) noexcept : bits_(bits) {}

    u64 bits_ = 0;
};

static_assert(sizeof(JobHandle) == 8, "a job handle is one machine word");

/// Nanoseconds on the monotonic clock. One reading, used by the scheduler, the watchdog and the
/// critical-path report, so that three subsystems cannot disagree about what "now" was.
i64 monotonic_now_ns() noexcept;

}  // namespace cy::jobs
