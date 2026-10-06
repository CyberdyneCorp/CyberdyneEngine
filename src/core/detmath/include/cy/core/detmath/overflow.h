// SPDX-License-Identifier: MIT
#pragma once
// Counting overflows without changing what overflow computes. Design §4.3.
//
// Overflow WRAPS, in every build. Saturation was rejected because it costs a branch on every
// operation and turns a bug into a plausible wrong value (a unit stuck at the map edge looks like
// gameplay); trapping was rejected because a development build and a shipping build would then
// compute different things. Wrapping is deterministic, so peers that overflow still agree.
//
// What a development build adds is NOTICING. `OverflowGuard` is modelled on
// `cy::determinism::NonFiniteGuard`: it counts, and it names the first site. The difference is how
// it is reached. A `NonFiniteGuard` is handed the value at the write; an overflow happens inside an
// operator that has no parameter to hand anything to, so a guard is installed for a scope instead,
// on the thread, and every wrapping operation on that thread reports to the innermost one.
//
//     detmath::OverflowGuard guard;
//     step_units(world);                   // Fixed arithmetic, all of it
//     if (guard.overflows() != 0) { log(guard.first_site()); }
//
// In Shipping (`kCountsOverflow` false) the operators do not check, so the guard counts nothing and
// costs nothing. With no guard installed, a development build checks and discards.

#include <cy/core/base/types.h>
#include <cy/core/detmath/config.h>

#include <type_traits>

namespace cy::detmath::inline CY_DETMATH_VARIANT {

/// Counts the overflows of every detmath operation on this thread while it is alive, and names the
/// first. Guards nest: the innermost one counts, and the outer one is restored when it ends.
class OverflowGuard {
public:
    /// Installs this guard as the thread's current one.
    OverflowGuard() noexcept;
    /// Restores the guard that was current before this one.
    ~OverflowGuard() noexcept;

    OverflowGuard(const OverflowGuard&) = delete;
    OverflowGuard& operator=(const OverflowGuard&) = delete;
    OverflowGuard(OverflowGuard&&) = delete;
    OverflowGuard& operator=(OverflowGuard&&) = delete;

    /// How many operations wrapped, saturated or left their domain since construction or `clear()`.
    [[nodiscard]] u64 overflows() const noexcept { return overflows_; }
    /// The operation that overflowed first, such as "Fixed *" or "sqrt of a negative". Empty when
    /// none has. The first rather than the last, because that is what a bisect wants.
    [[nodiscard]] const char* first_site() const noexcept { return first_site_; }
    /// Forget what was counted.
    void clear() noexcept;

    /// Report one overflow at `site` to the thread's current guard, if there is one.
    static void note(const char* site) noexcept;

private:
    OverflowGuard* previous_ = nullptr;
    u64 overflows_ = 0;
    const char* first_site_ = "";
};

namespace detail {

/// Called by every operation that wrapped. Compiled out where overflow is not counted, and never
/// called during constant evaluation, where a thread has no guard to report to.
constexpr void overflowed(const char* site) noexcept {
    if constexpr (kCountsOverflow) {
        if (!std::is_constant_evaluated()) {
            OverflowGuard::note(site);
        }
    } else {
        (void)site;
    }
}

}  // namespace detail

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
